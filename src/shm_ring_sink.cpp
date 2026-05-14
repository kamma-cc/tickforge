#include "tickforge/shm_ring_sink.hpp"
#include "tickforge/time.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>

namespace tickforge {

namespace {

bool is_pow2(size_t x) { return x >= 2 && (x & (x - 1)) == 0; }

[[noreturn]] void throw_errno(const char* what) {
    throw std::system_error(errno, std::generic_category(), what);
}

} // namespace

// ---------- ShmRingSink ----------

ShmRingSink::ShmRingSink(std::string name, size_t capacity, Policy policy)
    : name_(std::move(name)), policy_(policy) {
    if (name_.empty() || name_.front() != '/') {
        throw std::invalid_argument("shm name must start with '/'");
    }
    if (!is_pow2(capacity)) {
        throw std::invalid_argument("ShmRingSink capacity must be a power of two and >= 2");
    }

    total_size_ = sizeof(ShmRingHeader) + capacity * sizeof(ShmSlot);

    // Recreate fresh — if a previous run crashed and left a segment
    // behind, we own the name now.
    shm_unlink(name_.c_str());
    fd_ = shm_open(name_.c_str(), O_CREAT | O_RDWR | O_EXCL, 0600);
    if (fd_ < 0) throw_errno("shm_open");

    if (ftruncate(fd_, static_cast<off_t>(total_size_)) != 0) {
        int e = errno;
        ::close(fd_);
        shm_unlink(name_.c_str());
        errno = e;
        throw_errno("ftruncate");
    }

    base_ = mmap(nullptr, total_size_, PROT_READ | PROT_WRITE,
                 MAP_SHARED, fd_, 0);
    if (base_ == MAP_FAILED) {
        int e = errno;
        ::close(fd_);
        shm_unlink(name_.c_str());
        errno = e;
        throw_errno("mmap");
    }

    slots_ = reinterpret_cast<ShmSlot*>(static_cast<char*>(base_) + sizeof(ShmRingHeader));

    // The shm region is freshly zero-filled by the kernel (ftruncate of
    // a new segment), so we don't need to memset. Construct the header
    // in place so the atomics are well-initialized.
    hdr_ = ::new (base_) ShmRingHeader{};
    hdr_->magic         = ShmRingHeader::MAGIC;
    hdr_->version       = ShmRingHeader::VERSION;
    hdr_->capacity      = capacity;
    hdr_->capacity_mask = capacity - 1;
    hdr_->producer_seq.store(0, std::memory_order_relaxed);
    hdr_->consumer_seq.store(0, std::memory_order_relaxed);
}

ShmRingSink::~ShmRingSink() {
    if (base_ != nullptr && base_ != MAP_FAILED) {
        munmap(base_, total_size_);
    }
    if (fd_ >= 0) {
        ::close(fd_);
    }
    if (!name_.empty()) {
        shm_unlink(name_.c_str());
    }
}

bool ShmRingSink::push(const Event& ev, uint64_t send_ts_ns) {
    const uint64_t prod = hdr_->producer_seq.load(std::memory_order_relaxed);
    const uint64_t mask = hdr_->capacity_mask;

    // Wait / drop based on policy when the slot we're about to write is
    // still unread by the consumer.
    while (true) {
        const uint64_t cons = hdr_->consumer_seq.load(std::memory_order_acquire);
        if (prod - cons < hdr_->capacity) break;

        if (policy_ == Policy::DROP_IF_FULL) {
            ++dropped_;
            return false;
        }
        // BLOCK: spin and retry. We expect tight loops here; on an idle
        // box this can pin a core. If you don't want that, use DROP.
        cpu_pause();
    }

    ShmSlot& slot   = slots_[prod & mask];
    slot.ev         = ev;
    slot.send_ts_ns = send_ts_ns;

    hdr_->producer_seq.store(prod + 1, std::memory_order_release);
    return true;
}

// ---------- ShmRingConsumer ----------

ShmRingConsumer::ShmRingConsumer(std::string name) : name_(std::move(name)) {
    if (name_.empty() || name_.front() != '/') {
        throw std::invalid_argument("shm name must start with '/'");
    }

    fd_ = shm_open(name_.c_str(), O_RDWR, 0600);
    if (fd_ < 0) throw_errno("shm_open");

    // Map the header first to learn capacity, then remap the full thing.
    void* hdr_map = mmap(nullptr, sizeof(ShmRingHeader),
                         PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (hdr_map == MAP_FAILED) {
        ::close(fd_);
        throw_errno("mmap header");
    }
    auto* h = static_cast<ShmRingHeader*>(hdr_map);
    if (h->magic != ShmRingHeader::MAGIC || h->version != ShmRingHeader::VERSION) {
        munmap(hdr_map, sizeof(ShmRingHeader));
        ::close(fd_);
        throw std::runtime_error("shm ring magic/version mismatch — wrong segment?");
    }
    const uint64_t capacity = h->capacity;
    munmap(hdr_map, sizeof(ShmRingHeader));

    total_size_ = sizeof(ShmRingHeader) + capacity * sizeof(ShmSlot);
    base_ = mmap(nullptr, total_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (base_ == MAP_FAILED) {
        ::close(fd_);
        throw_errno("mmap full");
    }

    hdr_   = static_cast<ShmRingHeader*>(base_);
    slots_ = reinterpret_cast<ShmSlot*>(static_cast<char*>(base_) + sizeof(ShmRingHeader));
}

ShmRingConsumer::~ShmRingConsumer() {
    if (base_ != nullptr && base_ != MAP_FAILED) {
        munmap(base_, total_size_);
    }
    if (fd_ >= 0) {
        ::close(fd_);
    }
    // We do NOT shm_unlink — the producer owns the segment lifetime.
}

bool ShmRingConsumer::try_pop(Event& out_ev, uint64_t& out_send_ts) {
    const uint64_t cons = hdr_->consumer_seq.load(std::memory_order_relaxed);
    const uint64_t prod = hdr_->producer_seq.load(std::memory_order_acquire);
    if (cons == prod) return false;

    const ShmSlot& slot = slots_[cons & hdr_->capacity_mask];
    out_ev      = slot.ev;
    out_send_ts = slot.send_ts_ns;

    hdr_->consumer_seq.store(cons + 1, std::memory_order_release);
    return true;
}

} // namespace tickforge

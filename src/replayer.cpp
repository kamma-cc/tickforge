#include "tickforge/replayer.hpp"
#include "tickforge/time.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace tickforge {

namespace {

[[noreturn]] void throw_errno(const char* what) {
    throw std::system_error(errno, std::generic_category(), what);
}

} // namespace

// ---------- FileSink ----------

FileSink::FileSink(std::string path, size_t buffer_events)
    : path_(std::move(path)), cap_(buffer_events) {
    if (cap_ == 0) cap_ = 1;

    fd_ = ::open(path_.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd_ < 0) throw_errno("open");

    EventFileHeader hdr{};
    hdr.magic       = EventFileHeader::MAGIC;
    hdr.version     = EventFileHeader::VERSION;
    hdr.event_size  = sizeof(Event);
    hdr.event_count = 0;
    if (::write(fd_, &hdr, sizeof(hdr)) != sizeof(hdr)) {
        int e = errno;
        ::close(fd_);
        errno = e;
        throw_errno("write header");
    }
    buf_.reserve(cap_);
}

FileSink::~FileSink() {
    if (fd_ < 0) return;
    try { flush_buffer(); } catch (...) {}

    if (::lseek(fd_, 0, SEEK_SET) == 0) {
        EventFileHeader hdr{};
        hdr.magic       = EventFileHeader::MAGIC;
        hdr.version     = EventFileHeader::VERSION;
        hdr.event_size  = sizeof(Event);
        hdr.event_count = count_;
        ::write(fd_, &hdr, sizeof(hdr));
    }
    ::close(fd_);
}

bool FileSink::push(const Event& ev, uint64_t /*send_ts_ns*/) {
    buf_.push_back(ev);
    ++count_;
    if (buf_.size() >= cap_) flush_buffer();
    return true;
}

void FileSink::flush() { flush_buffer(); }

void FileSink::flush_buffer() {
    if (buf_.empty()) return;
    const size_t bytes = buf_.size() * sizeof(Event);
    const ssize_t w    = ::write(fd_, buf_.data(), static_cast<size_t>(bytes));
    if (w < 0 || static_cast<size_t>(w) != bytes) {
        throw_errno("write events");
    }
    buf_.clear();
}

// ---------- Replayer ----------

Replayer::Replayer(const std::string& path) {
    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0) throw_errno("open");

    struct stat st{};
    if (::fstat(fd_, &st) != 0) {
        int e = errno; ::close(fd_); errno = e;
        throw_errno("fstat");
    }
    total_size_ = static_cast<size_t>(st.st_size);
    if (total_size_ < sizeof(EventFileHeader)) {
        ::close(fd_);
        throw std::runtime_error("event file too small");
    }

    base_ = ::mmap(nullptr, total_size_, PROT_READ, MAP_SHARED, fd_, 0);
    if (base_ == MAP_FAILED) {
        int e = errno; ::close(fd_); errno = e;
        throw_errno("mmap");
    }

    hdr_ = static_cast<EventFileHeader*>(base_);
    if (hdr_->magic       != EventFileHeader::MAGIC   ||
        hdr_->version     != EventFileHeader::VERSION ||
        hdr_->event_size  != sizeof(Event)) {
        ::munmap(base_, total_size_);
        ::close(fd_);
        throw std::runtime_error("event file magic/version/size mismatch");
    }

    events_ = reinterpret_cast<Event*>(static_cast<char*>(base_) + sizeof(EventFileHeader));

    // Sequential read pattern — let the kernel readahead aggressively.
    ::madvise(base_, total_size_, MADV_SEQUENTIAL);
}

Replayer::~Replayer() {
    if (base_ != nullptr && base_ != MAP_FAILED) {
        ::munmap(base_, total_size_);
    }
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

uint64_t Replayer::replay(Sink& sink, Mode mode, double time_scale) {
    const uint64_t n = hdr_->event_count;
    if (n == 0) return 0;

    if (mode == Mode::MAX_THROUGHPUT) {
        for (uint64_t i = 0; i < n; ++i) {
            sink.push(events_[i], now_ns());
        }
        return n;
    }

    // WALL_CLOCK_REPLAY: spin-wait to the target wall time for each event.
    if (time_scale <= 0.0) time_scale = 1.0;
    const uint64_t origin0  = events_[0].origin_ts_ns;
    const uint64_t wall0    = now_ns();
    const double   inv_scale = 1.0 / time_scale;

    for (uint64_t i = 0; i < n; ++i) {
        const uint64_t ev_origin_delta = events_[i].origin_ts_ns - origin0;
        const uint64_t target_wall = wall0
            + static_cast<uint64_t>(static_cast<double>(ev_origin_delta) * inv_scale);

        while (now_ns() < target_wall) {
            cpu_pause();
        }
        sink.push(events_[i], now_ns());
    }
    return n;
}

} // namespace tickforge

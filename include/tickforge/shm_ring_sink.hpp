#pragma once

#include "event.hpp"
#include "sink.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace tickforge {

// Layout of the SPSC ring backed by POSIX shm. Producer (ShmRingSink)
// writes slots and bumps producer_seq; a separate consumer process
// (e.g. ShmRingConsumer) reads slots and bumps consumer_seq. The two
// counters live on separate cache lines to avoid false sharing.
struct ShmRingHeader {
    static constexpr uint64_t MAGIC   = 0x54494B4646524745ULL; // "TIKFFRGE"
    static constexpr uint64_t VERSION = 1;

    uint64_t magic;
    uint64_t version;
    uint64_t capacity;       // power of 2
    uint64_t capacity_mask;  // capacity - 1
    uint8_t  _pad0[64 - 32];

    alignas(64) std::atomic<uint64_t> producer_seq;
    uint8_t _pad1[64 - sizeof(std::atomic<uint64_t>)];

    alignas(64) std::atomic<uint64_t> consumer_seq;
    uint8_t _pad2[64 - sizeof(std::atomic<uint64_t>)];
};

static_assert(sizeof(ShmRingHeader) == 64 * 3,
              "ShmRingHeader layout drifted — check padding");

// One ring slot. We pay 128B per event (vs 64B for Event alone) so that
// each slot also carries the producer-side send_ts_ns. That preserves
// the three-clock decomposition (origin / send / recv) described in
// docs/design.md §9 even on the SHM path.
struct alignas(64) ShmSlot {
    Event    ev;
    uint64_t send_ts_ns;
    uint64_t _pad[7];
};
static_assert(sizeof(ShmSlot) == 128);

// SHM ring sink (producer side). Single-producer / single-consumer.
//
// Backpressure policies:
//   DROP_IF_FULL  — return false on push when the consumer hasn't drained;
//                   the dropped() counter increments. Default.
//   BLOCK         — spin-wait until the consumer drains. Use this only
//                   when you want to measure consumer throughput as the
//                   limiting factor; otherwise it ties producer rate to
//                   consumer rate.
class ShmRingSink : public Sink {
public:
    enum class Policy : uint8_t { DROP_IF_FULL = 0, BLOCK = 1 };

    // `name` must be a POSIX shm name and start with '/'.
    // `capacity` must be a power of two and at least 2.
    // If a previous run left a shm segment with the same name, it is
    // unlinked and recreated — we own the segment for our lifetime.
    ShmRingSink(std::string name, size_t capacity,
                Policy policy = Policy::DROP_IF_FULL);
    ~ShmRingSink() override;

    ShmRingSink(const ShmRingSink&)            = delete;
    ShmRingSink& operator=(const ShmRingSink&) = delete;

    bool push(const Event& ev, uint64_t send_ts_ns) override;

    uint64_t dropped() const { return dropped_; }
    const std::string& name() const { return name_; }

private:
    std::string         name_;
    Policy              policy_;
    int                 fd_         = -1;
    void*               base_       = nullptr;
    size_t              total_size_ = 0;
    ShmRingHeader*      hdr_        = nullptr;
    ShmSlot*            slots_      = nullptr;
    uint64_t            dropped_    = 0;
};

// Reader side. A separate process (or thread) maps the same shm by name
// and pulls events. Mirrors the SPSC contract: this class assumes it is
// the only consumer.
class ShmRingConsumer {
public:
    explicit ShmRingConsumer(std::string name);
    ~ShmRingConsumer();

    ShmRingConsumer(const ShmRingConsumer&)            = delete;
    ShmRingConsumer& operator=(const ShmRingConsumer&) = delete;

    // Returns true and fills `out_ev` / `out_send_ts` if a slot was read.
    // Returns false if the ring is empty (call again later).
    bool try_pop(Event& out_ev, uint64_t& out_send_ts);

    uint64_t capacity() const { return hdr_ ? hdr_->capacity : 0; }

private:
    std::string    name_;
    int            fd_         = -1;
    void*          base_       = nullptr;
    size_t         total_size_ = 0;
    ShmRingHeader* hdr_        = nullptr;
    ShmSlot*       slots_      = nullptr;
};

} // namespace tickforge

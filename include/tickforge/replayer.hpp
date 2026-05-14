#pragma once

#include "event.hpp"
#include "sink.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tickforge {

// On-disk header for an events file produced by FileSink. Keeping the
// header cache-line sized makes the rest of the file cleanly aligned at
// `sizeof(Event)` boundaries from offset 64.
struct EventFileHeader {
    static constexpr uint64_t MAGIC   = 0x54494B4646494C45ULL; // "TIKFFILE"
    static constexpr uint64_t VERSION = 1;

    uint64_t magic;
    uint64_t version;
    uint64_t event_size;   // = sizeof(Event); guards against layout drift
    uint64_t event_count;  // populated when FileSink finalizes
    uint8_t  _pad[64 - 32];
};
static_assert(sizeof(EventFileHeader) == 64);

// Sink that writes events sequentially to a binary file. Internal
// buffer batches writes to amortize syscall cost. Header `event_count`
// is written on close so a partially-written file (process crash) is
// still readable up to whatever made it to disk — the count tells the
// reader how many events to trust.
class FileSink : public Sink {
public:
    explicit FileSink(std::string path, size_t buffer_events = 16384);
    ~FileSink() override;

    FileSink(const FileSink&)            = delete;
    FileSink& operator=(const FileSink&) = delete;

    bool push(const Event& ev, uint64_t send_ts_ns) override;
    void flush() override;

    uint64_t count() const { return count_; }

private:
    void flush_buffer();

    std::string         path_;
    int                 fd_    = -1;
    size_t              cap_   = 0;
    std::vector<Event>  buf_;
    uint64_t            count_ = 0;
};

// Reads an events file via mmap and feeds it into a downstream Sink.
class Replayer {
public:
    enum class Mode : uint8_t {
        MAX_THROUGHPUT    = 0,
        WALL_CLOCK_REPLAY = 1,
    };

    explicit Replayer(const std::string& path);
    ~Replayer();

    Replayer(const Replayer&)            = delete;
    Replayer& operator=(const Replayer&) = delete;

    // Replay events into `sink`.
    //   MAX_THROUGHPUT     — push as fast as possible, no timing.
    //   WALL_CLOCK_REPLAY  — spin to maintain origin-timestamp deltas at
    //                        wall-clock rate. `time_scale` rescales delta
    //                        between events (2.0 → twice as fast).
    // Returns the number of events pushed.
    uint64_t replay(Sink& sink, Mode mode = Mode::MAX_THROUGHPUT,
                    double time_scale = 1.0);

    uint64_t event_count() const { return hdr_ ? hdr_->event_count : 0; }

private:
    int               fd_         = -1;
    void*             base_       = nullptr;
    size_t            total_size_ = 0;
    EventFileHeader*  hdr_        = nullptr;
    Event*            events_     = nullptr;
};

} // namespace tickforge

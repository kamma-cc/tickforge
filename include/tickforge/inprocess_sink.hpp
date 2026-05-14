#pragma once

#include "event.hpp"
#include "sink.hpp"

#include <cstdint>
#include <functional>

namespace tickforge {

// Header-only callback sink. The cheapest possible sink — just calls
// the user's lambda. Use this as the ground-truth baseline in §A
// microbenchmarks (see docs/design.md §2): if your system can't keep
// up with InProcessSink throughput, the bottleneck is in your code,
// not in any IPC layer.
class InProcessSink : public Sink {
public:
    using Callback = std::function<void(const Event&, uint64_t /*send_ts_ns*/)>;

    explicit InProcessSink(Callback cb) : cb_(std::move(cb)) {}

    bool push(const Event& ev, uint64_t send_ts_ns) override {
        cb_(ev, send_ts_ns);
        return true;
    }

private:
    Callback cb_;
};

} // namespace tickforge

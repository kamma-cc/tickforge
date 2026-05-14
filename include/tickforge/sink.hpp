#pragma once

#include "event.hpp"

#include <cstdint>

namespace tickforge {

// A Sink consumes events from the replayer / generator. Implementations
// decide their backpressure policy (drop / block / overwrite). See
// docs/design.md §7 for the policy table.
//
// The contract for `push`:
//   - Returns true if the event was accepted by the sink.
//   - Returns false if the sink rejected the event (drop policy) or could
//     not accept it without violating its policy (e.g. would have blocked
//     in non-blocking mode).
//   - `send_ts_ns` is the wall-clock timestamp (monotonic ns) at which the
//     producer is handing this event to the sink. The producer fills it,
//     not the sink. Used by downstream analysis to decompose latency into
//     producer-side jitter vs sink+link delay.
class Sink {
public:
    virtual ~Sink() = default;
    virtual bool push(const Event& ev, uint64_t send_ts_ns) = 0;
    virtual void flush() {}
};

} // namespace tickforge

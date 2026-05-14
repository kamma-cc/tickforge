#pragma once

#include <cstdint>
#include <ctime>

namespace tickforge {

// CLOCK_MONOTONIC_RAW is not subject to NTP slewing — what we want for
// latency measurement. It is Linux-specific; on other POSIX systems the
// caller can swap to CLOCK_MONOTONIC.
inline uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

// Hint to the CPU that we're in a tight spin loop. Lowers power and
// is friendlier to the sibling hyperthread. No-op fallback elsewhere.
inline void cpu_pause() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#endif
}

} // namespace tickforge

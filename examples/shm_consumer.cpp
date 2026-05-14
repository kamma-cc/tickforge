// shm_consumer — attach to a shm ring populated by tf_replay (or any
// ShmRingSink producer) and pull events as fast as possible. Reports
// throughput and average send→recv latency.

#include "tickforge/shm_ring_sink.hpp"
#include "tickforge/time.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace tickforge;

namespace {

std::atomic<bool> g_stop{false};
void on_sig(int) { g_stop.store(true); }

void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s /SHM_NAME [--max-events N] [--idle-ms N]\n"
        "  --max-events N    stop after receiving N events (0 = unlimited)\n"
        "  --idle-ms N       exit if no events for N ms (0 = wait forever)\n",
        prog);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { usage(argv[0]); return 2; }

    std::string name = argv[1];
    uint64_t    max_events      = 0;
    uint64_t    idle_ms_to_exit = 0;

    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--max-events" && i + 1 < argc) {
            max_events = std::strtoull(argv[++i], nullptr, 10);
        } else if (a == "--idle-ms" && i + 1 < argc) {
            idle_ms_to_exit = std::strtoull(argv[++i], nullptr, 10);
        } else if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            usage(argv[0]);
            return 2;
        }
    }

    std::signal(SIGINT,  on_sig);
    std::signal(SIGTERM, on_sig);

    ShmRingConsumer cons(name);
    std::fprintf(stderr, "shm_consumer: attached to %s capacity=%lu\n",
                 name.c_str(), (unsigned long)cons.capacity());

    Event    ev{};
    uint64_t send_ts      = 0;
    uint64_t n            = 0;
    uint64_t total_lat_ns = 0;
    uint64_t first_recv   = 0;
    uint64_t last_recv    = now_ns();
    uint64_t last_seq     = 0;
    uint64_t gap_count    = 0;
    bool     have_seq     = false;

    while (!g_stop.load(std::memory_order_relaxed)) {
        if (cons.try_pop(ev, send_ts)) {
            const uint64_t now = now_ns();
            if (n == 0) first_recv = now;
            total_lat_ns += (now - send_ts);

            if (have_seq && ev.seq != last_seq + 1) {
                ++gap_count;
            }
            last_seq = ev.seq;
            have_seq = true;

            ++n;
            last_recv = now;
            if (max_events && n >= max_events) break;
        } else {
            const uint64_t now = now_ns();
            if (idle_ms_to_exit &&
                (now - last_recv) / 1'000'000ULL > idle_ms_to_exit) {
                break;
            }
            cpu_pause();
        }
    }

    if (n > 0) {
        const double secs = (last_recv - first_recv) / 1e9;
        std::printf("shm_consumer:\n");
        std::printf("  received:               %lu events\n", (unsigned long)n);
        std::printf("  sequence gaps observed: %lu\n", (unsigned long)gap_count);
        std::printf("  active wall time:       %.3f ms\n", (last_recv - first_recv) / 1e6);
        std::printf("  throughput:             %.2f Mevt/s\n",
                    secs > 0 ? n / secs / 1e6 : 0.0);
        std::printf("  avg send→recv latency:  %.0f ns\n",
                    static_cast<double>(total_lat_ns) / n);
    } else {
        std::printf("shm_consumer: no events received\n");
    }
    return 0;
}

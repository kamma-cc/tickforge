// tf_replay — mmap a binary events file and replay it into a sink.
//
// Sinks supported:
//   --sink null    discard, useful for measuring pure replay throughput
//   --sink shm     write into a POSIX shm SPSC ring (see shm_consumer)

#include "tickforge/inprocess_sink.hpp"
#include "tickforge/replayer.hpp"
#include "tickforge/shm_ring_sink.hpp"
#include "tickforge/time.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace tickforge;

namespace {

void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s --in PATH --sink {null|shm} [options]\n"
        "  --in PATH                  events file (required)\n"
        "  --sink {null|shm}          sink kind (required)\n"
        "  --shm-name /NAME           POSIX shm name (required if sink=shm)\n"
        "  --shm-capacity N           ring slots, power of 2 (default 65536)\n"
        "  --shm-block                use BLOCK policy (default DROP_IF_FULL)\n"
        "  --mode {max|wall}          replay mode (default max)\n"
        "  --time-scale F             wall replay speed multiplier (default 1.0)\n",
        prog);
}

class NullSink : public Sink {
public:
    bool push(const Event&, uint64_t) override { ++n; return true; }
    uint64_t n = 0;
};

} // namespace

int main(int argc, char** argv) {
    std::string         in;
    std::string         sink_kind;
    std::string         shm_name;
    size_t              shm_capacity = 65536;
    bool                shm_block    = false;
    Replayer::Mode      mode         = Replayer::Mode::MAX_THROUGHPUT;
    double              time_scale   = 1.0;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need_next = [&]() -> const char* {
            if (i + 1 >= argc) { usage(argv[0]); std::exit(2); }
            return argv[++i];
        };
        if      (a == "--in")            in = need_next();
        else if (a == "--sink")          sink_kind = need_next();
        else if (a == "--shm-name")      shm_name = need_next();
        else if (a == "--shm-capacity")  shm_capacity = std::strtoull(need_next(), nullptr, 10);
        else if (a == "--shm-block")     shm_block = true;
        else if (a == "--mode") {
            std::string m = need_next();
            if (m == "max")       mode = Replayer::Mode::MAX_THROUGHPUT;
            else if (m == "wall") mode = Replayer::Mode::WALL_CLOCK_REPLAY;
            else { std::fprintf(stderr, "bad --mode: %s\n", m.c_str()); return 2; }
        }
        else if (a == "--time-scale")    time_scale = std::atof(need_next());
        else if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); usage(argv[0]); return 2; }
    }

    if (in.empty() || sink_kind.empty()) { usage(argv[0]); return 2; }

    Replayer rp(in);
    std::fprintf(stderr, "tf_replay: %lu events in %s\n",
                 (unsigned long)rp.event_count(), in.c_str());

    if (sink_kind == "null") {
        NullSink sink;
        const uint64_t t0 = now_ns();
        rp.replay(sink, mode, time_scale);
        const uint64_t t1 = now_ns();
        const double secs = (t1 - t0) / 1e9;
        std::fprintf(stderr,
            "tf_replay: pushed %lu in %.3f ms (%.2f Mevt/s)\n",
            (unsigned long)sink.n, (t1 - t0) / 1e6,
            secs > 0 ? sink.n / secs / 1e6 : 0.0);
        return 0;
    }

    if (sink_kind == "shm") {
        if (shm_name.empty()) { usage(argv[0]); return 2; }
        ShmRingSink sink(shm_name, shm_capacity,
                         shm_block ? ShmRingSink::Policy::BLOCK
                                   : ShmRingSink::Policy::DROP_IF_FULL);
        std::fprintf(stderr,
            "tf_replay: writing to shm %s capacity=%zu policy=%s\n",
            shm_name.c_str(), shm_capacity, shm_block ? "BLOCK" : "DROP_IF_FULL");
        const uint64_t t0 = now_ns();
        const uint64_t n  = rp.replay(sink, mode, time_scale);
        const uint64_t t1 = now_ns();
        const double secs = (t1 - t0) / 1e9;
        std::fprintf(stderr,
            "tf_replay: pushed %lu, dropped %lu in %.3f ms (%.2f Mevt/s)\n",
            (unsigned long)n, (unsigned long)sink.dropped(),
            (t1 - t0) / 1e6,
            secs > 0 ? n / secs / 1e6 : 0.0);
        return 0;
    }

    std::fprintf(stderr, "unknown sink: %s\n", sink_kind.c_str());
    usage(argv[0]);
    return 2;
}

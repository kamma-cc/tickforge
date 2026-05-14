// tf_generate — generate a binary events file using the MVP generator.
//
// Usage:
//   tf_generate --out events.bin [--symbols 4] [--duration-ms 1000]
//               [--step-ns 100000] [--vol 0.0001] [--trade-prob 0.01]
//               [--seed 0]

#include "tickforge/generator.hpp"
#include "tickforge/replayer.hpp"  // FileSink lives here

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace tickforge;

namespace {

void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s --out PATH [options]\n"
        "  --out PATH                 output events file (required)\n"
        "  --symbols N                number of symbols (default 4)\n"
        "  --duration-ms N            simulated duration in ms (default 1000)\n"
        "  --step-ns N                simulated time per step in ns (default 100000)\n"
        "  --vol F                    per-step log-return stddev (default 0.0001)\n"
        "  --trade-prob F             per-step trade probability (default 0.01)\n"
        "  --seed N                   RNG seed (default 0)\n",
        prog);
}

} // namespace

int main(int argc, char** argv) {
    GeneratorConfig cfg;
    std::string     out;
    uint64_t        duration_ms = 1000;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need_next = [&]() -> const char* {
            if (i + 1 >= argc) { usage(argv[0]); std::exit(2); }
            return argv[++i];
        };
        if      (a == "--out")          out = need_next();
        else if (a == "--symbols")      cfg.num_symbols       = static_cast<uint32_t>(std::atoi(need_next()));
        else if (a == "--duration-ms")  duration_ms           = std::strtoull(need_next(), nullptr, 10);
        else if (a == "--step-ns")      cfg.step_interval_ns  = std::strtoull(need_next(), nullptr, 10);
        else if (a == "--vol")          cfg.volatility        = std::atof(need_next());
        else if (a == "--trade-prob")   cfg.trade_prob_per_step = std::atof(need_next());
        else if (a == "--seed")         cfg.random_seed       = std::strtoull(need_next(), nullptr, 10);
        else if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); usage(argv[0]); return 2; }
    }
    if (out.empty()) { usage(argv[0]); return 2; }

    Generator gen(cfg);
    {
        FileSink sink(out);
        const uint64_t n = gen.run_for(duration_ms * 1'000'000ULL, sink);
        sink.flush();
        std::fprintf(stderr,
            "tf_generate: wrote %lu events (%lu symbols, %lu ms simulated) to %s\n",
            (unsigned long)n,
            (unsigned long)cfg.num_symbols,
            (unsigned long)duration_ms,
            out.c_str());
    }
    return 0;
}

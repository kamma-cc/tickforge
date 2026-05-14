// inprocess_demo — generate events directly into an InProcessSink and
// report throughput / event-type mix. This is the lowest-overhead path
// in tickforge and the right baseline before comparing any IPC sink.

#include "tickforge/generator.hpp"
#include "tickforge/inprocess_sink.hpp"
#include "tickforge/time.hpp"

#include <cstdio>
#include <cstdint>

using namespace tickforge;

int main() {
    GeneratorConfig cfg;
    cfg.num_symbols              = 8;
    cfg.step_interval_ns         = 100'000;     // 100 µs sim step
    cfg.trade_prob_per_step      = 0.05;
    cfg.book_delta_every_n_steps = 5;
    cfg.ticker_every_n_steps     = 50;

    uint64_t counts[4] = {0, 0, 0, 0};

    InProcessSink sink([&](const Event& e, uint64_t /*send_ts*/) {
        counts[static_cast<uint8_t>(e.type)]++;
    });

    Generator gen(cfg);

    const uint64_t sim_duration_ns = 1'000'000'000ULL; // 1 sim sec
    const uint64_t t0 = now_ns();
    const uint64_t n  = gen.run_for(sim_duration_ns, sink);
    const uint64_t t1 = now_ns();
    const double   secs = (t1 - t0) / 1e9;

    std::printf("inprocess_demo:\n");
    std::printf("  symbols:           %u\n", cfg.num_symbols);
    std::printf("  sim duration:      %.3f s\n", sim_duration_ns / 1e9);
    std::printf("  events emitted:    %lu\n", (unsigned long)n);
    std::printf("    TRADE:           %lu\n", (unsigned long)counts[(int)EventType::TRADE]);
    std::printf("    BOOK_DELTA:      %lu\n", (unsigned long)counts[(int)EventType::BOOK_DELTA]);
    std::printf("    TICKER:          %lu\n", (unsigned long)counts[(int)EventType::TICKER]);
    std::printf("  wall time:         %.3f ms\n", (t1 - t0) / 1e6);
    std::printf("  throughput:        %.2f Mevt/s\n",
                secs > 0 ? n / secs / 1e6 : 0.0);
    return 0;
}

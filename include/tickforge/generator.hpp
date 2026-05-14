#pragma once

#include "event.hpp"
#include "sink.hpp"

#include <cstdint>
#include <random>
#include <vector>

namespace tickforge {

// Knobs for the MVP generator. Defaults are chosen to produce a busy-
// looking stream (mix of trades, book deltas, tickers) for ~1 second
// of simulated time without explicit tuning.
//
// The model is intentionally simple — geometric-Brownian-motion price,
// per-step Bernoulli trade arrivals, lognormal trade sizes, and book
// updates on a coarse step counter. See docs/design.md §4.3 for the
// fidelity bar this implementation targets (and falls short of in MVP).
struct GeneratorConfig {
    uint32_t num_symbols              = 4;
    double   initial_price            = 100.0;
    double   volatility               = 1e-4;   // per-step log-return stddev
    double   trade_prob_per_step      = 0.01;   // P(trade) per symbol per step
    double   mean_trade_size          = 1.0;    // log-normal mean (linear units)
    double   stddev_log_trade_size    = 1.0;    // log-normal sigma
    int      ticker_every_n_steps     = 50;
    int      book_delta_every_n_steps = 5;
    double   tick_size                = 0.01;
    uint64_t random_seed              = 0;
    uint64_t step_interval_ns         = 100'000; // 100 µs simulated per step
};

// Generator runs a deterministic-given-seed market simulation and pushes
// events into a Sink as it goes. It does NOT throttle to wall-clock time
// — it generates as fast as it can. Use a Replayer if you want to play
// the resulting event stream back at a controlled rate.
class Generator {
public:
    explicit Generator(GeneratorConfig cfg);

    // Advance simulated time by `simulated_duration_ns`, pushing events
    // into `sink` as they arise. Returns the number of events emitted
    // (and accepted by the sink — drops do not count).
    uint64_t run_for(uint64_t simulated_duration_ns, Sink& sink);

    uint64_t total_emitted() const { return seq_; }

private:
    GeneratorConfig    cfg_;
    std::mt19937_64    rng_;
    std::vector<double> mid_;          // current mid price per symbol
    uint64_t           seq_           = 0;
    uint64_t           sim_time_ns_   = 0;
    uint64_t           step_counter_  = 0;
};

} // namespace tickforge

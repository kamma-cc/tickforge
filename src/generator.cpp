#include "tickforge/generator.hpp"
#include "tickforge/time.hpp"

#include <cmath>

namespace tickforge {

Generator::Generator(GeneratorConfig cfg) : cfg_(cfg), rng_(cfg.random_seed) {
    mid_.assign(cfg_.num_symbols, cfg_.initial_price);
}

uint64_t Generator::run_for(uint64_t simulated_duration_ns, Sink& sink) {
    const uint64_t end_time = sim_time_ns_ + simulated_duration_ns;
    const uint64_t step_dt  = cfg_.step_interval_ns;
    const double   tick     = cfg_.tick_size;

    std::normal_distribution<double>          norm(0.0, 1.0);
    std::uniform_real_distribution<double>    u01(0.0, 1.0);
    std::lognormal_distribution<double>       trade_size_dist(
        std::log(cfg_.mean_trade_size), cfg_.stddev_log_trade_size);

    uint64_t emitted = 0;

    auto snap_to_tick = [tick](double p) {
        return std::round(p / tick) * tick;
    };

    while (sim_time_ns_ < end_time) {
        sim_time_ns_ += step_dt;
        ++step_counter_;

        for (uint32_t s = 0; s < cfg_.num_symbols; ++s) {
            // Random-walk the mid (geometric Brownian).
            mid_[s] *= std::exp(cfg_.volatility * norm(rng_));
            if (mid_[s] < tick) mid_[s] = tick;

            // Trade?
            if (u01(rng_) < cfg_.trade_prob_per_step) {
                Event e{};
                e.origin_ts_ns = sim_time_ns_;
                e.seq          = seq_++;
                e.symbol_id    = s;
                e.type         = EventType::TRADE;
                e.side         = (u01(rng_) < 0.5) ? Side::BID : Side::ASK;
                e.action       = BookAction::UPSERT;
                e.px           = snap_to_tick(mid_[s]);
                e.qty          = trade_size_dist(rng_);
                e.extra1       = static_cast<double>(seq_); // dummy trade_id
                if (sink.push(e, now_ns())) ++emitted;
            }

            // Book delta? (coarse — see docs/design.md §11 fidelity TODO)
            if (cfg_.book_delta_every_n_steps > 0 &&
                step_counter_ % cfg_.book_delta_every_n_steps == 0) {
                const double offset = std::round(u01(rng_) * 5.0 + 1.0) * tick;
                const Side   side   = (u01(rng_) < 0.5) ? Side::BID : Side::ASK;
                const double px     = (side == Side::BID)
                    ? mid_[s] - offset
                    : mid_[s] + offset;
                const bool is_delete = u01(rng_) < 0.1;

                Event e{};
                e.origin_ts_ns = sim_time_ns_;
                e.seq          = seq_++;
                e.symbol_id    = s;
                e.type         = EventType::BOOK_DELTA;
                e.side         = side;
                e.action       = is_delete ? BookAction::DELETE : BookAction::UPSERT;
                e.px           = snap_to_tick(px);
                e.qty          = is_delete ? 0.0 : trade_size_dist(rng_) * 5.0;
                if (sink.push(e, now_ns())) ++emitted;
            }

            // Ticker?
            if (cfg_.ticker_every_n_steps > 0 &&
                step_counter_ % cfg_.ticker_every_n_steps == 0) {
                const double bid = std::floor(mid_[s] / tick) * tick;
                const double ask = bid + tick;

                Event e{};
                e.origin_ts_ns = sim_time_ns_;
                e.seq          = seq_++;
                e.symbol_id    = s;
                e.type         = EventType::TICKER;
                e.extra1       = bid;
                e.extra2       = ask;
                e.extra3       = trade_size_dist(rng_);
                if (sink.push(e, now_ns())) ++emitted;
            }
        }
    }

    return emitted;
}

} // namespace tickforge

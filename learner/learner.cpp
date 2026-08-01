#include "learner.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <future>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <databento/dbn.hpp>
#include <databento/dbn_file_store.hpp>
#include <databento/record.hpp>

#include "../core/logs.hpp"
#include "../core/market.hpp"
#include "../core/util/ta.hpp"
#include "../model/model.hpp"

// @TODO: long-only for now. shorting adds borrow costs that aren't modelled here.

static constexpr double price_scale = 1e-9;

// Boosting rounds per batch
static constexpr int training_rounds = 2952;

void learner::start(std::unique_ptr<model>& orama) {
    const auto now = std::chrono::system_clock::now();

    std::vector<std::string> paths;
    for (const auto& entry : std::filesystem::directory_iterator("raw_data")) {
        if (entry.path().extension() == ".zst") {
            paths.push_back(entry.path().string());
        }
    }

    std::ranges::shuffle(paths, std::mt19937{42});

    constexpr size_t batch_size = 8; // used to be 20 but ram limits

    for (size_t batch_start = 0; batch_start < paths.size(); batch_start += batch_size) {
        const size_t batch_end = std::min(batch_start + batch_size, paths.size());
        LOG_INFO(std::format("batch {}/{} — stocks {}-{}", batch_start / batch_size + 1,
                             (paths.size() + batch_size - 1) / batch_size, batch_start,
                             batch_end - 1));

        const auto batch_now = std::chrono::system_clock::now();
        // load this batch in parallel
        std::vector<std::future<std::vector<model::input>>> futures;
        for (size_t j = batch_start; j < batch_end; ++j) {
            const auto path = paths[j];
            futures.push_back(
                std::async(std::launch::async, [this, path] { return compute_data(path); }));
        }

        std::vector<model::input> inputs;
        for (auto& f : futures) {
            auto batch_inputs = f.get();
            inputs.insert(inputs.end(), std::make_move_iterator(batch_inputs.begin()),
                          std::make_move_iterator(batch_inputs.end()));
        }

        const auto batch_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now() - batch_now);
        LOG_INFO(std::format(
            "batch {}/{} — loaded {} inputs in {} ms", batch_start / batch_size + 1,
            (paths.size() + batch_size - 1) / batch_size, inputs.size(), batch_duration.count()));

        const auto now2 = std::chrono::system_clock::now();

        std::ranges::sort(inputs, {}, &model::input::ts_ns);

        const auto sort_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now() - now2);

        LOG_INFO(std::format(
            "batch {}/{} — sorted {} inputs in {} ms", batch_start / batch_size + 1,
            (paths.size() + batch_size - 1) / batch_size, inputs.size(), sort_duration.count()));

        size_t counts[3]{};
        for (const auto& inp : inputs) {
            if (inp.label) {
                ++counts[static_cast<int>(*inp.label)];
            }
        }
        LOG_INFO(std::format("batch class distribution — hold={} buy={} sell={}", counts[0],
                             counts[1], counts[2]));

        // Round count comes from the early-stopping search in model::learn(std::vector<input>),
        // which holds out the last 20% of each batch as a temporal validation split. Re-run
        // that overload after changing the feature set or the labelling thresholds, then set
        // this to the best_round it reports.
        orama->learn(std::move(inputs), training_rounds);
    }

    orama->save();

    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now() - now);
    LOG_INFO(std::format("total model training took {} ms", duration.count()));
}

std::vector<model::input> learner::compute_data(const std::string& path) {
    // get data from databento files

    const auto now = std::chrono::system_clock::now();

    auto intervals = get_data(path);

    size_t buy_count = 0;
    size_t sell_count = 0;
    size_t gap_skipped = 0;

    std::vector<model::input> inputs;

    const size_t span = static_cast<size_t>(config_.window_size) + config_.horizon;
    if (intervals.size() <= span) {
        LOG_WARNING(std::format("compute_data: {} has only {} bars (< {}), skipping", path,
                                intervals.size(), span));
        return inputs;
    }
    const size_t last_start = intervals.size() - span;

    for (size_t i = 0; i < last_start; ++i) {

        std::vector<market::interval> window(intervals.begin() + static_cast<ptrdiff_t>(i),
                                             intervals.begin() +
                                                 static_cast<ptrdiff_t>(i + config_.window_size));

        // Skip windows that span a session boundary
        // Consecutive 1-min bars should never be more than 2 minutes apart
        constexpr int64_t max_gap_ns = 2LL * 60 * 1'000'000'000;
        bool crosses_gap = false;
        for (size_t k = 0; k + 1 < window.size(); ++k) {
            if (window[k + 1].ts_ns - window[k].ts_ns > max_gap_ns) {
                crosses_gap = true;
                break;
            }
        }

        if (!crosses_gap) {
            // Start at window_size - 1: the boundary pair (last window bar -> first horizon
            // bar) must be gap-checked too, otherwise windows ending at a session close get
            // labeled with next-morning closes across the overnight gap.
            for (size_t k = i + config_.window_size - 1;
                 k + 1 < i + config_.window_size + config_.horizon; ++k) {
                if (intervals[k + 1].ts_ns - intervals[k].ts_ns > max_gap_ns) {
                    crosses_gap = true;
                    break;
                }
            }
        }

        if (crosses_gap) {
            ++gap_skipped;
            continue;
        }

        model::model_action label = model::model_action::hold;

        for (size_t j = i + config_.window_size; j < i + config_.window_size + config_.horizon;
             ++j) {
            const auto& future_bar = intervals[j];
            const auto& last_bar = window.back();

            const double gain = (future_bar.close - last_bar.close) / last_bar.close;

            const auto minimum_gain = config_.minimum_gain.value_or(0.001f);

            if (gain >= static_cast<double>(minimum_gain)) {
                label = model::model_action::buy;
                ++buy_count;
                break; // if we already know we want to buy, no need to check for sell
            }

            if (gain <= -static_cast<double>(minimum_gain)) {
                label = model::model_action::sell;
                ++sell_count;
                break; // if we already know we want to sell, no need to check for buy
            }
        }

        auto input_opt = prepare_input(window, label);
        if (input_opt.has_value()) {
            auto input = std::move(input_opt.value());
            input.ts_ns = window.back().ts_ns; // set the timestamp of the input to
                                               // the timestamp of the last bar in the window
            inputs.push_back(std::move(input));
        }
    }

    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now() - now);
    LOG_INFO(std::format("input computed for file {} took {} ms with {} buys and {} sells with "
                         "{} gaps skipped",
                         path, duration.count(), buy_count, sell_count, gap_skipped));

    return inputs;
}

std::vector<market::interval> learner::get_data(const std::string& path) {
    std::vector<market::interval> bars;
    auto store = databento::DbnFileStore(path);

    while (const auto* rec = store.NextRecord()) {
        if (rec->RType() != databento::RType::Ohlcv1M) {
            continue;
        }

        const auto& msg = rec->Get<databento::OhlcvMsg>();

        market::interval bar{};
        bar.ts_ns = static_cast<int64_t>(msg.hd.ts_event.time_since_epoch().count());
        bar.open = static_cast<double>(msg.open) * price_scale;
        bar.high = static_cast<double>(msg.high) * price_scale;
        bar.low = static_cast<double>(msg.low) * price_scale;
        bar.close = static_cast<double>(msg.close) * price_scale;
        bar.volume = static_cast<int64_t>(msg.volume);
        bar.datetime = market::to_ny(bar.ts_ns);
        bars.push_back(bar);
    }

    // only real time bars between 9:30 and 16:00, so filter out the rest
    std::erase_if(bars, [](const market::interval& b) {
        const auto dt = b.datetime;
        const bool before_open = dt.hour < 9 || (dt.hour == 9 && dt.minute < 30);
        const bool after_close = dt.hour >= 16;
        return before_open || after_close;
    });

    // sort them
    std::ranges::sort(bars, {}, &market::interval::ts_ns);

    util::ta::compute(bars);
    util::ta::drop_lookback(bars);

    return bars;
}

// assume intervals are sorted by timestamp ascending, and that the size of intervals is equal
// to config_.window_size
// @TODO: implment QQQ relative gain to stock (stock_return - qqq_return) as a feature, just make
// sure QQQ has the same timestamp as the stock!
// @TODO: implement FED data
std::optional<model::input> learner::prepare_input(const std::vector<market::interval>& intervals,
                                                   std::optional<model::model_action> label)

{
    model::input input{};

    if (intervals.size() != config_.window_size) { // need +1?
        LOG_ERROR(std::format("intervals size {} does not match config window size {}",
                              intervals.size(), config_.window_size));
        return std::nullopt;
    }

    input.label = label.value_or(model::model_action::hold);

    for (size_t i = 1; i < config_.window_size; ++i) {
        const auto& bar = intervals[i];
        const auto& prev_bar = intervals[i - 1];

        // simple ohlcv
        input.features.push_back(static_cast<float>(
            ((prev_bar.open != 0.0) ? (bar.open - prev_bar.open) / prev_bar.open : 0.0)));
        input.features.push_back(static_cast<float>(
            ((prev_bar.high != 0.0) ? (bar.high - prev_bar.high) / prev_bar.high : 0.0)));
        input.features.push_back(static_cast<float>(
            ((prev_bar.low != 0.0) ? (bar.low - prev_bar.low) / prev_bar.low : 0.0)));
        input.features.push_back(static_cast<float>(
            ((prev_bar.close != 0.0) ? (bar.close - prev_bar.close) / prev_bar.close : 0.0)));
        input.features.push_back(prev_bar.volume != 0
                                     ? static_cast<float>(bar.volume - prev_bar.volume) /
                                           static_cast<float>(prev_bar.volume)
                                     : 0.0F);

        // momentum
        input.features.push_back(static_cast<float>((bar.close - bar.open) / bar.open));

        // technicals which do not need normalization
        input.features.push_back(
            bar.technicals[static_cast<std::uint8_t>(market::indicators::rsi)].value());
        input.features.push_back(
            bar.technicals[static_cast<std::uint8_t>(market::indicators::mfi)].value());
        input.features.push_back(
            bar.technicals[static_cast<std::uint8_t>(market::indicators::aroon_down)].value());
        input.features.push_back(
            bar.technicals[static_cast<std::uint8_t>(market::indicators::aroon_up)].value());

        // normalized technicals
        float bb_lower = bar.technicals[util::ta::idx(market::indicators::bollinger_lower)].value();
        float bb_upper = bar.technicals[util::ta::idx(market::indicators::bollinger_upper)].value();
        float bb_mid = bar.technicals[util::ta::idx(market::indicators::bollinger_mid)].value();
        float bw = bb_upper - bb_lower;
        float safe_bw = bw > 1e-8f ? bw : 1.0f;

        input.features.push_back((static_cast<float>(bar.close) - bb_lower) / safe_bw); // %B
        input.features.push_back((static_cast<float>(bar.close) - bb_mid) /
                                 safe_bw); // distance from mid
        input.features.push_back((bb_upper - static_cast<float>(bar.close)) /
                                 safe_bw); // distance from upper

        // macd
        input.features.push_back(
            bar.technicals[static_cast<std::uint8_t>(market::indicators::macd)].value() /
            static_cast<float>(bar.close));
        input.features.push_back(
            bar.technicals[static_cast<std::uint8_t>(market::indicators::macd_signal)].value() /
            static_cast<float>(bar.close));
        input.features.push_back(
            bar.technicals[static_cast<std::uint8_t>(market::indicators::macd_histogram)].value() /
            static_cast<float>(bar.close));

        // ema momentum
        input.features.push_back(static_cast<float>(
            (bar.technicals[static_cast<std::uint8_t>(market::indicators::ema)].value() -
             prev_bar.technicals[static_cast<std::uint8_t>(market::indicators::ema)].value()) /
            prev_bar.technicals[static_cast<std::uint8_t>(market::indicators::ema)].value()));

        // difference between close and ema
        input.features.push_back(static_cast<float>(
            ((static_cast<float>(bar.close) -
              bar.technicals[static_cast<std::uint8_t>(market::indicators::ema)].value()) /
             bar.technicals[static_cast<std::uint8_t>(market::indicators::ema)].value())));

        // fama momentum
        input.features.push_back(static_cast<float>(
            (bar.technicals[static_cast<std::uint8_t>(market::indicators::fama)].value() -
             prev_bar.technicals[static_cast<std::uint8_t>(market::indicators::fama)].value()) /
            prev_bar.technicals[static_cast<std::uint8_t>(market::indicators::fama)].value()));

        // difference between close and fama
        input.features.push_back(static_cast<float>(
            (static_cast<float>(bar.close) -
             bar.technicals[static_cast<std::uint8_t>(market::indicators::fama)].value()) /
            bar.technicals[static_cast<std::uint8_t>(market::indicators::fama)].value()));

        // mama momentum
        input.features.push_back(static_cast<float>(
            (bar.technicals[static_cast<std::uint8_t>(market::indicators::mama)].value() -
             prev_bar.technicals[static_cast<std::uint8_t>(market::indicators::mama)].value()) /
            prev_bar.technicals[static_cast<std::uint8_t>(market::indicators::mama)].value()));

        // difference between close and mama
        input.features.push_back(static_cast<float>(
            (static_cast<float>(bar.close) -
             bar.technicals[static_cast<std::uint8_t>(market::indicators::mama)].value()) /
            bar.technicals[static_cast<std::uint8_t>(market::indicators::mama)].value()));

        // day and hour of day
        const auto ny_dt = bar.datetime;
        input.features.push_back(static_cast<float>(ny_dt.month));
        input.features.push_back(static_cast<float>(ny_dt.day));
        input.features.push_back(static_cast<float>(ny_dt.hour));
        input.features.push_back(static_cast<float>(ny_dt.minute));
    }

    return input;
}

void learner::once() {}
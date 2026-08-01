#include "manager.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <date/date.h>

#include "../core/logs.hpp"
#include "../core/market.hpp"
#include "../core/util/ta.hpp"
#include "../learner/learner.hpp"
#include "../model/model.hpp"

namespace {

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool is_rth(const market::ny_datetime& t) {
    const date::year_month_day ymd{date::year{t.year} / t.month / t.day};
    if (!ymd.ok()) {
        return false;
    }
    const date::weekday wd{date::sys_days{ymd}};
    if (wd == date::Saturday || wd == date::Sunday) {
        return false;
    }
    const int mins = t.hour * 60 + t.minute;
    return mins >= (9 * 60 + 30) && mins < (16 * 60);
}

bool bar_fresh(int64_t bar_open_ts_ns, int64_t now_ts_ns, int max_age_s, int bar_size_s = 60) {
    const int64_t close_ns = bar_open_ts_ns + static_cast<int64_t>(bar_size_s) * 1'000'000'000LL;
    const int64_t age = now_ts_ns - close_ns;
    return age >= -static_cast<int64_t>(bar_size_s) * 1'000'000'000LL &&
           age <= static_cast<int64_t>(max_age_s) * 1'000'000'000LL;
}

constexpr int64_t minute_ns = 60LL * 1'000'000'000;

size_t completed_bars(const std::vector<market::interval>& window, int64_t now_ts_ns) {
    size_t n = window.size();
    while (n > 0 && now_ts_ns < window[n - 1].ts_ns + minute_ns) {
        --n;
    }
    return n;
}

} // namespace

void manager::initialize(size_t max_target_count, std::chrono::minutes target_tracking_time) {
    max_target_count_ = max_target_count;
    target_tracking_time_ = target_tracking_time;

    // this will probally never happen but just in case
    if (max_target_count_ >= market::equity_list.size()) {
        throw std::runtime_error("max_target_count_ must be less than equity_list size");
    }

    gen_ = std::mt19937(rd_());

    learner_ = std::make_unique<learner>();

    learner_->set_config(orama_->get_config());

#ifdef TRAIN_MODEL
    learner_->start(orama_);
    return;
#endif

    me_ = std::make_unique<user>();

    me_->initialize();

    me_->set_order_callback([this](int id, const std::string& st, double price, int qty) {
        on_order_event(id, st, price, qty);
    });

    // check if we have open positions and add them internally
    const auto positions_opt = me_->positions();

    if (positions_opt && !positions_opt->empty()) {
        constexpr int restore_bars = util::ta::lookback_period * 2 + 16;
        for (const auto& p : *positions_opt) {
            position pos;
            pos.symbol = p.symbol;
            pos.entry_time = std::chrono::system_clock::now(); // this will do for now
            pos.entry_price = p.avg_cost;
            pos.quantity = static_cast<size_t>(p.quantity);

            auto hist = me_->historical(p.symbol, restore_bars);

            std::erase_if(hist, [](const market::interval& b) {
                const auto dt = b.datetime;
                const bool before_open = dt.hour < 9 || (dt.hour == 9 && dt.minute < 30);
                const bool after_close = dt.hour >= 16;
                return before_open || after_close;
            });
            util::ta::compute(hist);
            pos.window = std::move(hist);
            positions_.push_back(std::move(pos));
        }
    }

    targets_.reserve(max_target_count_);

    for (size_t i = 0; i < max_target_count_; ++i) {
        target t;
        update_target(&t);

        t.last_updated = std::chrono::system_clock::now() - std::chrono::seconds(10) +
                         std::chrono::milliseconds(i * 10'000 / max_target_count_);
        targets_.push_back(std::move(t));
    }
}

void manager::cycle() {
    if (wants_to_exit_) {
        {
            std::lock_guard<std::mutex> lk(positions_mutex_);
            for (const auto& p : positions_) {
                LOG_INFO(std::format("manager: shutting down with open {} qty={} stop_id={} "
                                     "take_id={} — leaving GTC brackets live",
                                     p.symbol, p.quantity, p.stop_id, p.take_id));
            }
        }
        can_exit_ = true;
        return;
    }

    if (!me_->connected()) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_reconnect_attempt_ >= reconnect_backoff_) {
            last_reconnect_attempt_ = now;
            LOG_WARNING(
                std::format("manager: broker disconnected, attempting reconnect (backoff {}s)",
                            reconnect_backoff_.count()));
            if (me_->connected() || me_->reconnect()) {
                LOG_INFO("manager: reconnected to broker");
                reconnect_backoff_ = reconnect_backoff_min;
                resubscribe_all();
            } else {
                reconnect_backoff_ =
                    std::min(reconnect_backoff_ * 2, std::chrono::seconds(reconnect_backoff_max));
            }
        }
        return; // skip this cycle try again next tick
    }

    expire_pending_orders();

    // sync internal positions against live ibkr state
    if (const auto acc = me_->account()) {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        latest_account_ = *acc;
    }

    const auto ibkr_positions_opt = me_->positions();
    if (ibkr_positions_opt) {
        const auto& ibkr_positions = *ibkr_positions_opt;
        const auto now_recon = std::chrono::system_clock::now();
        std::lock_guard<std::mutex> lk(positions_mutex_);
        for (auto& p : positions_) {
            const auto it =
                std::ranges::find_if(ibkr_positions, [&p](const broker::position_info& ip) {
                    return ip.symbol == p.symbol;
                });
            if (it == ibkr_positions.end()) {
                if (p.state != pos_state::closed && now_recon - p.entry_time > reconcile_grace) {
                    LOG_WARNING(std::format(
                        "manager: {} no longer at IBKR ({}), removing from tracking", p.symbol,
                        p.state == pos_state::closing ? "close confirmed" : "closed externally"));
                    p.state = pos_state::closed;
                }
                continue;
            }
            if (p.state == pos_state::closing) {
                if (now_recon - p.close_requested > close_retry) {
                    LOG_WARNING(std::format(
                        "manager: close of {} unconfirmed after {} min, resuming management",
                        p.symbol, close_retry.count()));
                    p.state = pos_state::open;
                }
                continue;
            }

            if (static_cast<size_t>(it->quantity) != p.quantity) {
                LOG_WARNING(std::format("manager: {} qty mismatch internal={} ibkr={}, syncing",
                                        p.symbol, p.quantity, static_cast<size_t>(it->quantity)));
                p.quantity = static_cast<size_t>(it->quantity);
            }
            p.entry_price = it->avg_cost;
            p.unrealized_pnl = it->unrealized_pnl;
        }
    } else {
        LOG_WARNING("manager: broker positions unknown (disconnected) — skipping reconcile, "
                    "keeping tracked positions");
    }

    std::vector<int> dead_subs;
    {
        std::lock_guard<std::mutex> lk(positions_mutex_);
        for (const auto& p : positions_) {
            if (p.state == pos_state::closed && p.req_id >= 0) {
                dead_subs.push_back(p.req_id);
            }
        }
        std::erase_if(positions_, [](const position& p) { return p.state == pos_state::closed; });
    }
    for (const int id : dead_subs) {
        me_->cancel_subscription(id);
    }

    const int64_t now_ts = now_ns();
    const market::ny_datetime now_ny = market::to_ny(now_ts);
    if (!is_rth(now_ny)) {
        return;
    }

    update_targets();

    constexpr size_t waittime = 10;   // seconds between predictions (targets and positions)
    constexpr int max_bar_age_s = 90; // bars older than this after close are treated as stale

    auto now_sys = std::chrono::system_clock::now();

    std::vector<entry_candidate> entries;

    for (auto& t : targets_) {
        std::lock_guard<std::mutex> lk(targets_mtx_);

        if ((now_sys - t.last_updated < std::chrono::seconds(waittime)) && !t.window.empty()) {
            continue;
        }
        if (t.window.empty()) {
            continue;
        }

        t.last_updated = now_sys;

        const size_t n_done = completed_bars(t.window, now_ts);
        if (n_done == 0) {
            continue;
        }
        const auto& last_done = t.window[n_done - 1];

        if (!bar_fresh(last_done.ts_ns, now_ts, max_bar_age_s)) {
            LOG_WARNING(std::format("manager: stale bar for {} (age > {}s), skipping", t.symbol,
                                    max_bar_age_s));
            continue;
        }

        // Skip if symbol already has an open position or pending order
        {
            std::lock_guard<std::mutex> plk(positions_mutex_);
            if (std::ranges::any_of(positions_,
                                    [&](const position& p) { return p.symbol == t.symbol; }) ||
                std::ranges::any_of(pending_orders_,
                                    [&](const auto& kv) { return kv.second.symbol == t.symbol; })) {
                continue;
            }
        }

        util::ta::compute(t.window);

        const auto pred = predict({t.window.data(), n_done});

        if (pred) {
            t.confidence = pred->confidence;
            t.last_action = pred->action;
            t.action_probs = pred->action_probs;
        }

        //@TODO: remove or rewrite this, just placeholder
        {
            std::lock_guard<std::mutex> stats_lk(stats_mtx_);
            if (pred) {
                if (pred->action == model::model_action::buy) {
                    ++stat_buys_;
                } else if (pred->action == model::model_action::sell) {
                    ++stat_sells_;
                } else if (pred->action == model::model_action::hold) {
                    ++stat_holds_;
                }

                stat_total_buy_ += pred->action_probs[1];
                stat_total_sell_ += pred->action_probs[2];
                stat_total_hold_ += pred->action_probs[0];

                stat_highest_buy_ = std::max(pred->action_probs[1], stat_highest_buy_);
                stat_highest_sell_ = std::max(pred->action_probs[2], stat_highest_sell_);
                stat_highest_hold_ = std::max(pred->action_probs[0], stat_highest_hold_);
                ++stat_times_counted_;
            } else {
                ++stat_holds_;
            }
        }

        if (pred && pred->action == model::model_action::buy) {
            LOG_INFO(std::format("manager: model suggests buying {}, confidence {:.2f}%", t.symbol,
                                 pred->confidence * 100));
            entries.push_back(
                {.symbol = t.symbol,
                 .confidence = pred->confidence,
                 .price = last_done.close,
                 .window = std::vector<market::interval>(
                     t.window.begin(), t.window.begin() + static_cast<std::ptrdiff_t>(n_done))});
        }
    }

    {
        std::lock_guard<std::mutex> lk(entry_candidates_mtx_);
        entry_candidates_ = entries;
    }

    for (auto& e : entries) {
        make_position(std::move(e));
    }

    const auto stat_duration = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now() - stat_last_time_);
    if (stat_duration.count() >= 120) {
        stat_last_time_ = std::chrono::system_clock::now();
        logs::log_stats(stat_buys_, stat_sells_, stat_holds_, stat_highest_hold_, stat_highest_buy_,
                        stat_highest_sell_, stat_total_hold_, stat_total_buy_, stat_total_sell_,
                        stat_times_counted_);
    }

    {
        std::lock_guard<std::mutex> lk(positions_mutex_);
        for (auto& p : positions_) {
            if (p.req_id < 0) {
                me_->request_subscription(p.symbol, &p.req_id);
            }
        }
    }

    // Predict on each position's live window and collect close requests.
    struct close_req {
        std::string symbol;
        size_t qty;
    };

    constexpr std::chrono::minutes max_tracking_time =
        std::chrono::minutes(30); // @TODO: hypertune?

    // update it incase we spent a long time in the above loop
    now_sys = std::chrono::system_clock::now();

    std::vector<close_req> to_close;
    {
        std::lock_guard<std::mutex> lk(positions_mutex_);
        for (auto& p : positions_) {
            if (p.state != pos_state::open) {
                continue; // close in flight or confirmed never issue a second SELL
            }
            if (p.window.empty() || p.quantity == 0) {
                continue;
            }
            if (now_sys - p.last_updated < std::chrono::seconds(waittime)) {
                continue;
            }
            p.last_updated = now_sys;

            const size_t n_done = completed_bars(p.window, now_ts);
            if (n_done == 0) {
                continue;
            }
            if (!bar_fresh(p.window[n_done - 1].ts_ns, now_ts, max_bar_age_s)) {
                LOG_WARNING(std::format("manager: stale bar for position {} (age > {}s), skipping",
                                        p.symbol, max_bar_age_s));
                continue;
            }

            util::ta::compute(p.window);

            const auto tracking_time =
                std::chrono::duration_cast<std::chrono::minutes>(now_sys - p.entry_time);

            const auto pred = predict({p.window.data(), n_done});
            const bool model_exit = pred && pred->action == model::model_action::sell;
            const bool timeout_exit = tracking_time >= max_tracking_time;
            if (model_exit || timeout_exit) {
                if (model_exit) {
                    LOG_INFO(std::format("manager: model suggests selling {}, confidence {:.2f}%",
                                         p.symbol, pred->confidence * 100));
                } else {
                    LOG_INFO(std::format("manager: closing {} after {} min tracking timeout",
                                         p.symbol, tracking_time.count()));
                }
                to_close.push_back({p.symbol, p.quantity});
                p.state = pos_state::closing; // reconcile confirms removal (or retries the close)
                p.close_requested = now_sys;
            }
        }
    }

    for (const auto& cr : to_close) {
        const int cancelled = me_->cancel_open_orders(cr.symbol);
        me_->place_market_order(cr.symbol, "SELL", static_cast<double>(cr.qty));
        LOG_INFO(std::format("manager: closing {} qty={} (cancelled {} resting order(s))",
                             cr.symbol, cr.qty, cancelled));
    }
}

void manager::end() {
    orama_->save();

#ifndef TRAIN_MODEL
    me_->end();
#endif
}

void manager::update_targets() {
    for (auto& t : targets_) {
        const auto now = std::chrono::system_clock::now();
        if (now - t.entry_time > target_tracking_time_) {
            update_target(&t);
            continue;
        }
        bool occupied = false; // removes if we have a open positon or a open order for this symbol
        {
            std::lock_guard<std::mutex> lk(positions_mutex_);
            occupied =
                std::ranges::any_of(positions_,
                                    [&t](const position& p) { return p.symbol == t.symbol; }) ||
                std::ranges::any_of(pending_orders_,
                                    [&t](const auto& kv) { return kv.second.symbol == t.symbol; });
        }
        if (occupied) {
            update_target(&t);
        }
    }
}

void manager::update_target(target* t) {
    int old_req_id;
    {
        std::lock_guard<std::mutex> lk(targets_mtx_);
        old_req_id = t->req_id;
        t->req_id = -1;
        t->entry_time = std::chrono::system_clock::now();
        t->last_updated = {};
        t->window.clear();
    }

    if (old_req_id >= 0) {
        me_->cancel_subscription(old_req_id);
    }

    std::string equity = market::equity_list[random_index(market::equity_list.size())];

    std::vector<std::string> occupied_syms;
    {
        std::lock_guard<std::mutex> lk(positions_mutex_);
        occupied_syms.reserve(positions_.size());
        for (const auto& p : positions_) {
            occupied_syms.push_back(p.symbol);
        }
    }

    size_t attempts = 0;
    while (std::ranges::any_of(targets_,
                               [&equity](const target& it) { return it.symbol == equity; }) ||
           std::ranges::find(occupied_syms, equity) != occupied_syms.end()) {
        if (++attempts > market::equity_list.size() * 2) {
            LOG_ERROR("update_target: equity list exhausted, no unique symbol available");
            return;
        }
        equity = market::equity_list[random_index(market::equity_list.size())];
    }

    constexpr size_t n = util::ta::lookback_period * 2 + 6;
    auto hist = me_->historical(equity, n);
    if (!hist.empty()) {
        std::erase_if(hist, [](const market::interval& b) {
            const auto dt = b.datetime;
            const bool before_open = dt.hour < 9 || (dt.hour == 9 && dt.minute < 30);
            const bool after_close = dt.hour >= 16;
            return before_open || after_close;
        });
    }

    int new_req_id = -1;
    me_->request_subscription(equity, &new_req_id);
    LOG_INFO(
        std::format("manager: updated target {}, got {} historical bars", equity, hist.size()));

    {
        std::lock_guard<std::mutex> lk(targets_mtx_);
        t->symbol = equity;
        t->window = std::move(hist);
        t->req_id = new_req_id;
    }
}

void manager::make_position(entry_candidate&& cand) {
    constexpr size_t max_open_positions = 3;
    constexpr double max_drawdown = 0.015; // 1.5% stop loss
    constexpr double take_profit = 0.02;   // 2% take profit

    if (!entries_allowed()) {
        return;
    }

    const double price = cand.price;
    if (!(price > 0.0)) {
        LOG_WARNING(std::format("manager: non-positive price for {}, skipping", cand.symbol));
        return;
    }

    double committed = 0.0;
    {
        std::lock_guard<std::mutex> lk(positions_mutex_);
        if (positions_.size() + pending_orders_.size() >= max_open_positions) {
            return;
        }
        if (std::ranges::any_of(positions_,
                                [&](const position& p) { return p.symbol == cand.symbol; })) {
            return;
        }
        if (std::ranges::any_of(pending_orders_,
                                [&](const auto& kv) { return kv.second.symbol == cand.symbol; })) {
            return;
        }
        committed = committed_usd_locked();
    }

    const size_t quantity =
        me_->determine_quantity(price, cand.confidence, max_drawdown, committed);
    if (quantity == 0) {
        LOG_WARNING(std::format("manager: zero quantity for {}, skipping", cand.symbol));
        return;
    }

    const double stop_price = price * (1.0 - max_drawdown);
    const double take_price = price * (1.0 + take_profit);

    LOG_INFO(std::format("manager: placing MKT bracket {}", cand.symbol));
    const auto ids = me_->place_bracket_order(cand.symbol, "BUY", static_cast<double>(quantity),
                                              price, stop_price, take_price);
    if (ids.parent_id < 0) {
        return; // broker refused (no valid order id yet) nothing was placed
    }
    {
        std::lock_guard<std::mutex> lk(positions_mutex_);
        pending_orders_[ids.parent_id] = {.symbol = cand.symbol,
                                          .requested_qty = quantity,
                                          .stop_id = ids.stop_id,
                                          .take_id = ids.take_id,
                                          .entry_price = price,
                                          .created = std::chrono::system_clock::now(),
                                          .seed_window = std::move(cand.window),
                                          .confidence = cand.confidence,
                                          .stop_price = stop_price,
                                          .take_price = take_price};
    }
    ++trades_today_; // main-thread only; counts toward the daily cap
    LOG_INFO(std::format("manager: bracket {} qty={} entry={:.4f} sl={:.4f} tp={:.4f}", cand.symbol,
                         quantity, price, stop_price, take_price));
}

void manager::on_order_event(int order_id, const std::string& status, double avg_fill_price,
                             int filled_qty) {
    std::lock_guard<std::mutex> lk(positions_mutex_);

    if (status == "Filled" && filled_qty > 0) {
        for (auto& p : positions_) {
            if ((order_id == p.stop_id || order_id == p.take_id) && p.state != pos_state::closed) {
                LOG_INFO(std::format("manager: bracket exit leg {} filled for {} @ {:.4f}",
                                     order_id, p.symbol, avg_fill_price));
                p.state = pos_state::closed;
                return; // exit-leg ids are never pending parent ids
            }
        }
    }

    auto it = pending_orders_.find(order_id);
    if (it == pending_orders_.end()) {
        return;
    }
    auto& po = it->second;

    if (status == "Cancelled" || status == "ApiCancelled" || status == "Inactive") {
        LOG_WARNING(std::format("manager: order {} for {} {}", order_id, po.symbol, status));
        pending_orders_.erase(it);
        return;
    }
    if (filled_qty <= 0) {
        return;
    }

    auto new_qty = static_cast<size_t>(filled_qty);

    auto pos_it =
        std::ranges::find_if(positions_, [&](const position& p) { return p.symbol == po.symbol; });
    if (pos_it == positions_.end()) {
        position p;
        p.symbol = po.symbol;
        p.entry_time = std::chrono::system_clock::now();
        p.entry_price = avg_fill_price;
        p.quantity = new_qty;
        p.stop_id = po.stop_id;
        p.take_id = po.take_id;
        p.confidence = po.confidence;
        p.stop_price = po.stop_price;
        p.take_price = po.take_price;
        p.window = std::move(po.seed_window); // seeded from target window at order time
        positions_.push_back(std::move(p));
    } else {
        pos_it->quantity = std::max(pos_it->quantity, new_qty);

        if (avg_fill_price > 0.0) {
            pos_it->entry_price = avg_fill_price;
        }
    }

    if (status == "Filled" && new_qty >= po.requested_qty) {
        LOG_INFO(std::format("manager: order filled {} qty={}", po.symbol, new_qty));
        pending_orders_.erase(it);
    }
}

double manager::committed_usd_locked() const {
    const auto now = std::chrono::system_clock::now();
    double sum = 0.0;
    for (const auto& p : positions_) {
        if (now - p.entry_time <= cash_settle_grace) {
            sum += static_cast<double>(p.quantity) * p.entry_price;
        }
    }
    for (const auto& [id, po] : pending_orders_) {
        sum += static_cast<double>(po.requested_qty) * po.entry_price;
    }
    return sum;
}

void manager::resubscribe_all() {
    {
        std::lock_guard<std::mutex> lk(targets_mtx_);
        for (auto& t : targets_) {
            if (t.req_id >= 0) {
                t.req_id = -1;
                me_->request_subscription(t.symbol, &t.req_id);
            }
        }
    }
    {
        std::lock_guard<std::mutex> lk(positions_mutex_);
        for (auto& p : positions_) {
            if (p.req_id >= 0) {
                p.req_id = -1;
                me_->request_subscription(p.symbol, &p.req_id);
            }
        }
    }
    LOG_INFO("manager: re-subscribed all real-time bar streams after reconnect");
}

bool manager::entries_allowed() {
    if (kill_switch_.load(std::memory_order_relaxed)) {
        return false;
    }

    const market::ny_datetime now_ny = market::to_ny(now_ns());
    const int day = (now_ny.year * 10000) + (now_ny.month * 100) + now_ny.day;
    const double equity = me_->usd_account_value();

    // Reset per-day state on a new NY trading day.
    if (day != risk_day_) {
        risk_day_ = day;
        trades_today_ = 0;
        day_start_equity_ = equity;
        halted_logged_ = false;
    }

    if (day_start_equity_ <= 0.0 && equity > 0.0) {
        day_start_equity_ = equity;
    }

    if (day_start_equity_ > 0.0 && equity > 0.0 &&
        equity < day_start_equity_ * (1.0 - max_daily_loss_frac)) {
        if (!halted_logged_) {
            LOG_ERROR(std::format("manager: daily loss limit hit (start={:.2f} now={:.2f} USD), "
                                  "halting new entries for the day",
                                  day_start_equity_, equity));
            halted_logged_ = true;
        }
        return false;
    }

    if (trades_today_ >= max_trades_per_day) {
        return false;
    }
    return true;
}

void manager::expire_pending_orders() {
    std::vector<int> to_cancel; // parent ids
    {
        std::lock_guard<std::mutex> lk(positions_mutex_);
        const auto now = std::chrono::system_clock::now();
        for (const auto& [pid, po] : pending_orders_) {
            if (now - po.created >= std::chrono::seconds(pending_order_ttl_secs)) {
                to_cancel.push_back(pid);
            }
        }
    }
    for (const int pid : to_cancel) {
        me_->cancel_order(pid); // cancelling the parent cancels its attached children
        LOG_WARNING(std::format("manager: pending order {} unfilled after {}s, cancelling", pid,
                                pending_order_ttl_secs));
    }
}

std::optional<manager::prediction> manager::predict(std::span<const market::interval> window) {
    if (!orama_) {
        return std::nullopt;
    }

    const auto window_size = static_cast<std::size_t>(orama_->get_config().window_size);
    if (window.size() < window_size) {
        return std::nullopt; // warm-up not enough completed bars yet, treat as hold
    }

    std::vector<market::interval> sliced(window.end() - static_cast<std::ptrdiff_t>(window_size),
                                         window.end());

    constexpr int64_t max_gap_ns = 2LL * 60 * 1'000'000'000;
    for (size_t k = 0; k + 1 < sliced.size(); ++k) {
        if (sliced[k + 1].ts_ns - sliced[k].ts_ns > max_gap_ns) {
            return std::nullopt;
        }
    }

    util::ta::drop_lookback(sliced); // it needs techncials, so this is safety
    if (sliced.size() != window_size) {
        return std::nullopt; // technicals not warmed up over the full slice yet — hold
    }
    auto inp = learner_->prepare_input(sliced);
    if (!inp) {
        return std::nullopt;
    }

    const std::array<float, 3> probs = orama_->predict(*inp);
    const float best = std::max({probs[0], probs[1], probs[2]});

    prediction pred{};
    pred.confidence = best;
    pred.action_probs = probs;
    pred.action = model::model_action::hold; // default to hold

    if (best < orama_->get_config().minimum_confidence) {
        return pred;
    }

    model::model_action action = model::model_action::hold;
    if (best == probs[1]) {
        action = model::model_action::buy;
    } else if (best == probs[2]) {
        action = model::model_action::sell;
    }

    return prediction{.action = action, .confidence = best, .action_probs = probs};
}

void manager::on_new_bar(const market::interval& update, const int id) {
    auto merge = [&](std::vector<market::interval>& window) {
        auto same = std::ranges::find_if(window, [&](const market::interval& b) {
            return b.datetime.year == update.datetime.year &&
                   b.datetime.month == update.datetime.month &&
                   b.datetime.day == update.datetime.day &&
                   b.datetime.hour == update.datetime.hour &&
                   b.datetime.minute == update.datetime.minute;
        });
        if (same != window.end()) {
            same->close = update.close;
            same->high = std::max(update.high, same->high);
            same->low = std::min(update.low, same->low);
            same->volume += update.volume;
        } else {
            if (update.ts_ns % (60LL * 1'000'000'000) != 0) {
                LOG_DEBUG(std::format("manager: dropping mid-minute first sub-bar (ts_ns={})",
                                      update.ts_ns));
                return;
            }
            window.push_back(update);
            if (window.size() > max_window_bars) {
                window.erase(window.begin(), window.begin() + static_cast<std::ptrdiff_t>(
                                                                  window.size() - max_window_bars));
            }
        }
    };

    {
        std::lock_guard<std::mutex> lk(targets_mtx_);
        auto find = std::ranges::find_if(targets_, [&](const target& t) { return t.req_id == id; });
        if (find != targets_.end()) {
            merge(find->window);
            return;
        }
    }

    {
        std::lock_guard<std::mutex> lk(positions_mutex_);
        auto it =
            std::ranges::find_if(positions_, [&](const position& p) { return p.req_id == id; });
        if (it != positions_.end()) {
            merge(it->window);
            return;
        }
    }

    LOG_WARNING(std::format("manager: received new bar for unknown req_id {}, ignoring", id));
}
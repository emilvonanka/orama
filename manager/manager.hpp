#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../core/broker/broker.hpp"
#include "../core/market.hpp"
#include "../learner/learner.hpp"
#include "../model/model.hpp"
#include "../user/user.hpp"

class manager {
public:
    static constexpr std::size_t max_window_bars = 128;

    struct target {
        target() = default;
        target(std::string symbol) : symbol(std::move(symbol)) {

            this->entry_time = std::chrono::system_clock::now();
            window.clear(); // just in case
        }
        std::string symbol;
        std::chrono::system_clock::time_point entry_time; // when we started targeting this equity
        std::vector<market::interval>
            window;
        std::chrono::system_clock::time_point
            last_updated; // when we last got a new bar for this equity
        int req_id = -1;  // the request id for the live bars subscription, so we can unsubscribe
                          // on target removal
        float confidence = 0.0F; // last prediction's confidence, for the UI
        model::model_action last_action = model::model_action::hold;
        std::array<float, 3> action_probs{}; // [hold, buy, sell], for the UI
    };

    enum class pos_state : std::uint8_t { open, closing, closed };

    struct position {
        std::string symbol;
        std::chrono::system_clock::time_point entry_time;
        double entry_price = 0.0;
        size_t quantity = 0;
        int stop_id = -1;
        int take_id = -1;
        int req_id = -1; // real-time bars subscription; -1 = not yet subscribed
        pos_state state = pos_state::open;
        std::chrono::system_clock::time_point close_requested{}; // set when state -> closing
        std::vector<market::interval>
            window;
        std::chrono::system_clock::time_point
            last_updated;            // when we last predicted (throttle, not last bar received)
        float confidence = 0.0F;     // model confidence at entry, for the UI
        double stop_price = 0.0;     // for the UI
        double take_price = 0.0;     // for the UI
        double unrealized_pnl = 0.0; // synced from IBKR during reconcile, for the UI
    };

    struct prediction {
        model::model_action action;
        float confidence;
        std::array<float, 3> action_probs; // [hold, buy, sell]
    };

    void create_model(const model::config& conf,
                      const std::vector<std::pair<std::string, std::string>>& params) {
        orama_ = std::make_unique<model>(conf, params);
    }

    void initialize(size_t max_target_count, std::chrono::minutes target_tracking_time);

    void cycle();

    void end();

    [[nodiscard]] bool running() const {
        return !(wants_to_exit_ && can_exit_);
    }

    void want_exit() {
        wants_to_exit_ = true;
    }

    void on_new_bar(const market::interval& update, const int id);

    struct pending_order {
        std::string symbol;
        size_t requested_qty;
        int stop_id;
        int take_id;
        double entry_price = 0.0;                     
        std::chrono::system_clock::time_point created;
        std::vector<market::interval> seed_window;     // target window snapshot, applied on fill
        float confidence = 0.0F; // model confidence at decision time, for the UI
        double stop_price = 0.0; // for the UI
        double take_price = 0.0; // for the UI
    };

    struct entry_candidate {
        std::string symbol;
        float confidence = 0.0F;
        double price = 0.0;                   // close of the last completed bar
        std::vector<market::interval> window; // snapshot incl. the forming bar (seeds the position)
    };

    struct stats_snapshot {
        size_t buys = 0;
        size_t sells = 0;
        size_t holds = 0;
        float highest_buy = 0.0F;
        float highest_sell = 0.0F;
        float highest_hold = 0.0F;
        float avg_buy = 0.0F;
        float avg_sell = 0.0F;
        float avg_hold = 0.0F;
        size_t times_counted = 0;
        double net_liquidation = 0.0;
        double unrealized_pnl = 0.0;
        double realized_pnl = 0.0;
    };

    [[nodiscard]] std::vector<target> get_targets() const {
        std::lock_guard<std::mutex> lk(targets_mtx_);
        return targets_;
    }
    [[nodiscard]] std::vector<position> get_positions() const {
        std::lock_guard<std::mutex> lk(positions_mutex_);
        return positions_;
    }
    [[nodiscard]] std::vector<pending_order> get_pending_orders() const {
        std::lock_guard<std::mutex> lk(positions_mutex_);
        std::vector<pending_order> out;
        out.reserve(pending_orders_.size());
        for (const auto& [id, po] : pending_orders_) {
            out.push_back(po);
        }
        return out;
    }
    [[nodiscard]] std::vector<entry_candidate> get_entry_candidates() const {
        std::lock_guard<std::mutex> lk(entry_candidates_mtx_);
        return entry_candidates_;
    }
    [[nodiscard]] stats_snapshot get_stats() const {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        stats_snapshot s{};
        s.buys = stat_buys_;
        s.sells = stat_sells_;
        s.holds = stat_holds_;
        s.highest_buy = stat_highest_buy_;
        s.highest_sell = stat_highest_sell_;
        s.highest_hold = stat_highest_hold_;
        s.times_counted = stat_times_counted_;
        if (stat_times_counted_ > 0) {
            s.avg_buy = stat_total_buy_ / static_cast<float>(stat_times_counted_);
            s.avg_sell = stat_total_sell_ / static_cast<float>(stat_times_counted_);
            s.avg_hold = stat_total_hold_ / static_cast<float>(stat_times_counted_);
        }
        if (latest_account_) {
            s.net_liquidation = latest_account_->net_liquidation;
            s.unrealized_pnl = latest_account_->unrealized_pnl;
            s.realized_pnl = latest_account_->realized_pnl;
        }
        return s;
    }

private:
    size_t random_index(size_t size) {
        std::uniform_int_distribution<size_t> dist(0, size - 1);
        return dist(gen_);
    }

    void update_targets();
    void update_target(target* t);

    void make_position(entry_candidate&& cand);
    void on_order_event(int order_id, const std::string& status, double avg_fill_price,
                        int filled_qty);

    void resubscribe_all();


    bool entries_allowed();
  
    [[nodiscard]] double committed_usd_locked() const;

    void expire_pending_orders();

    [[nodiscard]] std::optional<prediction> predict(std::span<const market::interval> window);

    std::vector<target>
        targets_; // current equities we are targeting, will be updated by the learner and used by
                  // the manager to decide what positions to open/close
    std::vector<position> positions_; // current positions we have open
    mutable std::mutex targets_mtx_;  // guards targets_[*].window and targets_[*].req_id
    mutable std::mutex positions_mutex_;
    std::unordered_map<int, pending_order> pending_orders_; // keyed by bracket parent_id
    std::vector<entry_candidate> entry_candidates_; // last cycle's buy candidates, for the UI
    mutable std::mutex entry_candidates_mtx_;
    std::atomic<bool> can_exit_ = false;
    std::atomic<bool> wants_to_exit_ = false; // will be true when user wants to exit
    std::unique_ptr<model> orama_ = nullptr;
    std::unique_ptr<learner> learner_ = nullptr;
    std::unique_ptr<user> me_ = nullptr;
    size_t max_target_count_;
    std::chrono::minutes target_tracking_time_;
    std::random_device rd_;
    std::mt19937 gen_;

    static constexpr size_t max_trades_per_day = 100;   // hard cap on new entries per day
    static constexpr double max_daily_loss_frac = 0.05; // halt new entries past 5% daily drawdown
    static constexpr int pending_order_ttl_secs = 120;  // cancel an unfilled parent after this
    static constexpr std::chrono::minutes reconcile_grace{3};
    // If a close order hasn't confirmed after this, re-open the position for management (the
    // SELL may have been rejected/lost) instead of leaving it
    static constexpr std::chrono::minutes close_retry{2};
    static constexpr std::chrono::minutes cash_settle_grace{5};
    std::atomic<bool> kill_switch_{false}; // manual emergency stop for new entries

    static constexpr std::chrono::seconds reconnect_backoff_min{5};
    static constexpr std::chrono::seconds reconnect_backoff_max{60};
    std::chrono::steady_clock::time_point last_reconnect_attempt_{};
    std::chrono::seconds reconnect_backoff_{reconnect_backoff_min};
    int risk_day_ = 0; // NY day (YYYYMMDD) the counters belong to
    size_t trades_today_ = 0;
    double day_start_equity_ = 0.0;
    bool halted_logged_ = false;

    // UI
    mutable std::mutex stats_mtx_;
    std::optional<broker::account_info> latest_account_; // guarded by stats_mtx_
    size_t stat_buys_ = 0;
    size_t stat_sells_ = 0;
    size_t stat_holds_ = 0;
    float stat_highest_buy_ = 0.0F;
    float stat_highest_sell_ = 0.0F;
    float stat_highest_hold_ = 0.0F;
    size_t stat_times_counted_ = 0;
    float stat_total_buy_ = 0.0F;
    float stat_total_sell_ = 0.0F;
    float stat_total_hold_ = 0.0F;
    std::chrono::system_clock::time_point stat_last_time_;
};

namespace orama {
inline std::unique_ptr<manager> head = nullptr;
}
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// ibkr api
#include "../market.hpp"
#include "CommissionAndFeesReport.h"
#include "Contract.h"
#include "Decimal.h"
#include "EClientSocket.h"
#include "EReader.h"
#include "EReaderOSSignal.h"
#include "EWrapper.h"
#include "Execution.h"
#include "Order.h"
#include "OrderCancel.h"
#include "OrderState.h"
#include "bar.h"

class broker : public EWrapper {
public:
    struct account_info {
        std::string account;
        double net_liquidation = 0;
        double buying_power = 0;
        double available_funds = 0;
        double cash_balance = 0;
        double unrealized_pnl = 0;
        double realized_pnl = 0;
    };

    struct position_info {
        std::string symbol;
        double quantity = 0;
        double avg_cost = 0;
        double market_value = 0;
        double unrealized_pnl = 0;
    };

    struct bracket_ids {
        int parent_id, stop_id, take_id;
    };

    // order_id, status string, avg fill price, filled quantity
    using order_cb_t = std::function<void(int, const std::string&, double, int)>;

    broker();
    ~broker();

    bool connect(const std::string& host = "127.0.0.1", int port = 7497, int client_id = 0);
    void disconnect();

    bool reconnect();
    [[nodiscard]] bool connected() const {
        return connected_.load();
    }

    void set_order_callback(order_cb_t cb) {
        std::lock_guard<std::mutex> lk(order_cb_mtx_);
        order_cb_ = std::move(cb);
    }

    void cancel_real_time_bars(int id);

    std::optional<account_info> get_account_info() const;

    std::optional<std::vector<position_info>> get_positions() const;
    std::vector<market::interval> req_historical(const std::string& symbol, int n_bars);
    std::optional<double> req_fx_rate(const std::string& base, const std::string& quote);

    int place_market_order(const std::string& symbol, const std::string& action, double qty);
    int place_limit_order(const std::string& symbol, const std::string& action, double qty,
                          double lmt);
    int place_stop_order(const std::string& symbol, const std::string& action, double qty,
                         double stop);
    int place_stop_limit_order(const std::string& symbol, const std::string& action, double qty,
                               double lmt, double stop);
    int place_trailing_stop(const std::string& symbol, const std::string& action, double qty,
                            double trail_pct);
    bracket_ids place_bracket_order(const std::string& symbol, const std::string& action,
                                    double qty, double entry_lmt, double stop_price,
                                    double take_price, bool mkt_entry = true);
    bool cancel_order(int order_id);

    int cancel_open_orders(const std::string& symbol);

    void request_subscription(const std::string& symbol, int* req_id);

private:
    static Contract make_stock_contract(const std::string& symbol);
    static Contract make_forex_contract(const std::string& base, const std::string& quote);
    static market::interval bar_to_interval(const Bar& bar);

    static int64_t ibkr_time_to_ns(const std::string& t);

    static bool sanitize_interval(market::interval& iv);

    static double round_to_tick(double price);

    int alloc_req_id() {
        return next_req_id_.fetch_add(1);
    }

    int alloc_order_id() {
        if (next_order_id_.load() < 0) {
            return -1;
        }
        return next_order_id_.fetch_add(1);
    }

    [[nodiscard]] order_cb_t order_callback() const {
        std::lock_guard<std::mutex> lk(order_cb_mtx_);
        return order_cb_;
    }

    struct pending_hist {
        std::mutex mtx;
        std::condition_variable cv;
        std::vector<market::interval> bars;
        bool done{false};
    };

    struct pending_connect {
        std::mutex mtx;
        std::condition_variable cv;
        bool done{false};
    };

    struct pending_account_ready {
        std::mutex mtx;
        std::condition_variable cv;
        bool done{false};
    };

    EReaderOSSignal signal_;
    std::unique_ptr<EClientSocket> client_;
    std::unique_ptr<EReader> reader_;
    std::thread reader_thread_;
    std::atomic<bool> connected_{false};

    std::string host_;
    int port_ = 0;
    int client_id_ = 0;

    mutable std::mutex account_mutex_;
    std::string account_name_;
    bool account_subscribed_{false};
    account_info cached_account_;
    std::vector<position_info> cached_positions_;

    pending_connect pending_connect_;
    pending_account_ready account_ready_;

    pending_connect order_id_ready_;

    std::atomic<int> next_order_id_{-1};
    static constexpr int req_id_base = 1'000'000;
    std::atomic<int> next_req_id_{req_id_base};

    mutable std::mutex live_orders_mtx_;
    std::unordered_map<int, std::string> live_orders_;

    std::mutex hist_map_mtx_;
    std::unordered_map<int, std::shared_ptr<pending_hist>> pending_hist_map_;

    struct fx_entry {
        double rate = 0.0;
        std::chrono::steady_clock::time_point ts;
    };
    mutable std::mutex fx_mtx_;
    std::unordered_map<std::string, fx_entry> fx_cache_;
    static constexpr std::chrono::seconds fx_ttl{60};

    mutable std::mutex order_cb_mtx_;
    order_cb_t order_cb_;

// declare all EWrapper pure virtual overrides via macro trick
#undef EWRAPPER_VIRTUAL_IMPL
#define EWRAPPER_VIRTUAL_IMPL override
#include "EWrapper_prototypes.h"
};

void broker_example();

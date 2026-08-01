#pragma once
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../core/broker/broker.hpp"
#include "../core/market.hpp"
class user {
public:
    user() = default;
    void initialize();

    void end();

    [[nodiscard]] std::optional<broker::account_info> account() const {
        return ibkr_->get_account_info();
    };

    // nullopt = broker state unknown (disconnected/not downloaded)
    [[nodiscard]] std::optional<std::vector<broker::position_info>> positions() const {
        return ibkr_->get_positions();
    }

    int cancel_open_orders(const std::string& symbol) {
        return ibkr_->cancel_open_orders(symbol);
    }

    [[nodiscard]] std::vector<market::interval> historical(const std::string& symbol,
                                                           int const n) const {
        return ibkr_->req_historical(symbol, n);
    }

    void set_order_callback(broker::order_cb_t cb);

    [[nodiscard]] bool connected() const {
        return ibkr_->connected();
    }
    bool reconnect() {
        return ibkr_->reconnect();
    }

    // limit order
    int place_order(const std::string& symbol, const std::string& side, double qty, double lmt);
    int place_stoploss(const std::string& symbol, const std::string& side, double qty, double stop);

    broker::bracket_ids place_bracket_order(const std::string& symbol, const std::string& side,
                                            double qty, double entry_lmt, double stop_price,
                                            double take_price);
    int place_market_order(const std::string& symbol, const std::string& side, double qty);
    void cancel_order(int order_id);

    size_t determine_quantity(double price, float confidence, double stop_loss_pct,
                              double committed_usd);

    [[nodiscard]] double usd_account_value() const;

    void cancel_subscription(int req_id) {
        if (req_id < 0) {
            return;
        }
        ibkr_->cancel_real_time_bars(req_id);
    }

    void request_subscription(const std::string& symbol, int* req_id);

private:
    [[nodiscard]] double usd_available_funds() const; // free cash in USD
    [[nodiscard]] std::optional<double> account_in_usd(double base_amount) const;

    std::unique_ptr<broker> ibkr_;
};
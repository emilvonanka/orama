#include "user.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "../core/broker/broker.hpp"
#include "../core/logs.hpp"

void user::initialize() {
    ibkr_ = std::make_unique<broker>();

    if (!ibkr_->connect("127.0.0.1", 7497, 1)) { // default TWS paper trading port and client id
        LOG_ERROR("user: failed to connect to ibkr");
        return;
    }
}

void user::end() {
    ibkr_->disconnect();
}

int user::place_order(const std::string& symbol, const std::string& side, double qty, double lmt) {
    return ibkr_->place_limit_order(symbol, side, qty, lmt);
}

int user::place_stoploss(const std::string& symbol, const std::string& side, double qty,
                         double stop) {
    return ibkr_->place_stop_order(symbol, side, qty, stop);
}

void user::set_order_callback(broker::order_cb_t cb) {
    ibkr_->set_order_callback(std::move(cb));
}

broker::bracket_ids user::place_bracket_order(const std::string& symbol, const std::string& side,
                                              double qty, double entry_lmt, double stop_price,
                                              double take_price) {
    return ibkr_->place_bracket_order(symbol, side, qty, entry_lmt, stop_price, take_price);
}

int user::place_market_order(const std::string& symbol, const std::string& side, double qty) {
    return ibkr_->place_market_order(symbol, side, qty);
}

void user::cancel_order(int order_id) {
    ibkr_->cancel_order(order_id);
}

// The IBKR account is denominated in SEK; positions are USD.
std::optional<double> user::account_in_usd(double base_amount) const {
    const auto rate = ibkr_->req_fx_rate("USD", "SEK");
    if (!rate || *rate <= 0.0) {
        LOG_ERROR("user: failed to get USD/SEK rate");
        return std::nullopt;
    }
    return base_amount / *rate;
}

double user::usd_available_funds() const {
    const auto me = account();
    if (!me) {
        LOG_ERROR("user: cannot get account info for FX conversion");
        return 0.0;
    }
    return account_in_usd(me->cash_balance).value_or(0.0);
}

double user::usd_account_value() const {
    const auto me = account();
    if (!me) {
        LOG_ERROR("user: cannot get account info for sizing");
        return 0.0;
    }
    return account_in_usd(me->net_liquidation).value_or(0.0);
}

// Position sizing model (all amounts in USD):
// committed_usd is notional reserved by working orders not yet reflected in cash, supplied by
// the manager which knows open positions + pending orders.
size_t user::determine_quantity(double price, float confidence, double stop_loss_pct,
                                double committed_usd) {
    if (price <= 0.0 || stop_loss_pct <= 0.0 || !(confidence > 0.0F)) {
        return 0;
    }

    const double account_value = usd_account_value(); // total equity
    const double cash = usd_available_funds();        // free cash
    if (account_value <= 0.0 || cash <= 0.0) {
        return 0;
    }

    const double remaining_cash = cash - committed_usd;
    if (remaining_cash <= 0.0) {
        LOG_WARNING("user: no uncommitted cash left for a new position");
        return 0;
    }

    constexpr double max_account_fraction = 0.10; // <= 10% of equity in any single trade
    constexpr double max_risk_per_trade = 0.05;   // <= 5% of equity lost if the stop fires

    // 1) Conviction-scaled notional cap, 2) bounded by cash actually available.
    const double notional_cap =
        account_value * max_account_fraction * static_cast<double>(confidence);
    const double notional = std::min(notional_cap, remaining_cash);
    if (notional <= 0.0) {
        return 0;
    }

    // 3) Stop-distance risk bound.
    const double risk_per_share = price * stop_loss_pct;
    const double risk_qty = (account_value * max_risk_per_trade) / risk_per_share;

    const double qty = std::min(notional / price, risk_qty);
    if (qty < 1.0) {
        return 0;
    }
    return static_cast<size_t>(qty);
}

void user::request_subscription(const std::string& symbol, int* req_id) {
    if (req_id == nullptr) {
        LOG_ERROR("user: request_subscription called with null req_id pointer");
        return;
    }
    ibkr_->request_subscription(symbol, req_id);
}
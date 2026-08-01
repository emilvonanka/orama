#include "broker.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../../manager/manager.hpp"
#include "../logs.hpp"
#include "../market.hpp"
#include "Contract.h"
#include "Decimal.h"
#include "EClientSocket.h"
#include "Execution.h"
#include "Order.h"
#include "TagValue.h"

broker::broker() : signal_(2000) {
    client_ = std::make_unique<EClientSocket>(this, &signal_);
}

broker::~broker() {
    disconnect();
}

bool broker::connect(const std::string& host, int port, int client_id) {
    host_ = host;
    port_ = port;
    client_id_ = client_id;
    {
        std::lock_guard<std::mutex> lk(pending_connect_.mtx);
        pending_connect_.done = false;
    }
    {
        std::lock_guard<std::mutex> lk(account_ready_.mtx);
        account_ready_.done = false;
    }
    {
        std::lock_guard<std::mutex> lk(order_id_ready_.mtx);
        order_id_ready_.done = false;
    }

    next_order_id_.store(-1);

    client_->eConnect(host.c_str(), port, client_id);
    std::this_thread::sleep_for(
        std::chrono::seconds(2)); // brief pause to allow connection to establish
    if (!client_->isConnected()) {
        LOG_ERROR(std::format("IBKR: failed to connect to {}:{}", host, port));
        return false;
    }

    reader_ = std::make_unique<EReader>(client_.get(), &signal_);
    reader_->start();

    connected_ = true;

    reader_thread_ = std::thread([this] {
        while (connected_) {
            signal_.waitForSignal();
            if (!connected_) {
                break;
            }

            try {
                reader_->processMsgs();
            } catch (const std::exception& e) {
                LOG_ERROR(std::format("IBKR: exception in reader processMsgs: {}", e.what()));
            } catch (...) {
                LOG_ERROR("IBKR: unknown exception in reader processMsgs");
            }
        }
    });

    {
        std::unique_lock<std::mutex> lk(pending_connect_.mtx);
        bool ok = pending_connect_.cv.wait_for(lk, std::chrono::seconds(5),
                                               [this] { return pending_connect_.done; });
        if (!ok) {
            LOG_WARNING("IBKR: timed out waiting for managed accounts");
        }
    }

    {
        std::unique_lock<std::mutex> lk(order_id_ready_.mtx);
        bool ok = order_id_ready_.cv.wait_for(lk, std::chrono::seconds(5),
                                              [this] { return order_id_ready_.done; });
        if (!ok) {
            LOG_WARNING(
                "IBKR: timed out waiting for nextValidId — orders disabled until it arrives");
        }
    }

    // Subscribe to account updates immediately
    {
        std::lock_guard<std::mutex> lk(account_mutex_);
        client_->reqAccountUpdates(true, account_name_);
        account_subscribed_ = true;
    }

    {
        std::unique_lock<std::mutex> lk(account_ready_.mtx);
        bool ok = account_ready_.cv.wait_for(lk, std::chrono::seconds(30),
                                             [this] { return account_ready_.done; });
        if (!ok) {
            LOG_WARNING("IBKR: timed out waiting for initial account data");
        }
    }

    client_->reqAllOpenOrders();
    std::this_thread::sleep_for(std::chrono::seconds(1)); // brief window for openOrder callbacks

    LOG_INFO(std::format("IBKR: connected to {}:{} account={}", host, port, account_name_));
    return true;
}

void broker::disconnect() {
    if (!client_) {
        return;
    }

    const bool was_connected = connected_.exchange(false);

    signal_.issueSignal();

    if (was_connected && client_->isConnected()) {
        std::lock_guard<std::mutex> lk(account_mutex_);
        if (account_subscribed_) {
            client_->reqAccountUpdates(false, account_name_);
        }
        account_subscribed_ = false;
        client_->eDisconnect();
    }

    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }

    if (was_connected) {
        LOG_INFO("IBKR: disconnected");
    }
}

bool broker::reconnect() {
    LOG_WARNING("IBKR: reconnecting");

    disconnect();
    reader_.reset();

    client_ = std::make_unique<EClientSocket>(this, &signal_);

    {
        std::lock_guard<std::mutex> lk(account_mutex_);
        cached_positions_.clear();
        account_subscribed_ = false;
    }
    {
        std::lock_guard<std::mutex> lk(live_orders_mtx_);
        live_orders_.clear();
    }

    return connect(host_, port_, client_id_);
}

// ============================================================
// Static helpers
// ============================================================

Contract broker::make_stock_contract(const std::string& symbol) {
    Contract c;
    c.symbol = symbol;
    c.secType = "STK";
    c.exchange = "SMART";
    c.primaryExchange = "NASDAQ"; // @TODO: should fix some issue with "GOOGL" and "GOOG"
    c.currency = "USD";
    return c;
}

Contract broker::make_forex_contract(const std::string& base, const std::string& quote) {
    Contract c;
    c.symbol = base;
    c.secType = "CASH";
    c.exchange = "IDEALPRO";
    c.currency = quote;
    return c;
}

int64_t broker::ibkr_time_to_ns(const std::string& t) {
    errno = 0;
    char* end = nullptr;
    const long long secs = std::strtoll(t.c_str(), &end, 10);

    constexpr long long min_secs = 946'684'800LL;
    constexpr long long max_secs = 4'102'444'800LL;
    if (end == t.c_str() || *end != '\0' || errno == ERANGE || secs < min_secs || secs > max_secs) {
        LOG_WARNING(std::format("IBKR: unparseable bar time '{}', skipping bar", t));
        return -1;
    }
    return secs * 1'000'000'000LL;
}

double broker::round_to_tick(double price) {
    if (!std::isfinite(price) || price <= 0.0) {
        return price;
    }
    const double tick = (price >= 1.0) ? 0.01 : 0.0001;
    return std::round(price / tick) * tick;
}

bool broker::sanitize_interval(market::interval& iv) {
    const bool prices_ok = std::isfinite(iv.open) && std::isfinite(iv.high) &&
                           std::isfinite(iv.low) && std::isfinite(iv.close) && iv.open > 0.0 &&
                           iv.high > 0.0 && iv.low > 0.0 && iv.close > 0.0 && iv.high >= iv.low;
    if (!prices_ok) {
        return false;
    }
    if (iv.volume < 0) {
        iv.volume = 0; // IBKR reports -1 for no volume data
    }
    return true;
}

market::interval broker::bar_to_interval(const Bar& bar) {
    market::interval iv{};
    iv.ts_ns = ibkr_time_to_ns(bar.time);
    iv.open = bar.open;
    iv.high = bar.high;
    iv.low = bar.low;
    iv.close = bar.close;
    iv.volume = static_cast<int64_t>(DecimalFunctions::decimalToDouble(bar.volume));
    if (iv.ts_ns >= 0) {
        iv.datetime = market::to_ny(iv.ts_ns);
    }

    return iv;
}

// ============================================================
// Market data
// ============================================================

std::vector<market::interval> broker::req_historical(const std::string& symbol, int n_bars) {
    const int days = (n_bars / 390) + 3;
    const std::string duration = std::format("{} D", days);

    auto state = std::make_shared<pending_hist>();
    int req_id = alloc_req_id();

    {
        std::lock_guard<std::mutex> lk(hist_map_mtx_);
        pending_hist_map_[req_id] = state;
    }

    client_->reqHistoricalData(req_id, make_stock_contract(symbol), "", duration, "1 min", "TRADES",
                               1, 2, false, TagValueListSPtr());

    {
        std::unique_lock<std::mutex> lk(state->mtx);
        bool ok =
            state->cv.wait_for(lk, std::chrono::seconds(10), [&state] { return state->done; });
        if (!ok) {
            LOG_WARNING(std::format("IBKR: req_historical {} timed out", symbol));
        }
    }

    {
        std::lock_guard<std::mutex> lk(hist_map_mtx_);
        pending_hist_map_.erase(req_id);
    }

    std::vector<market::interval> bars;
    {
        std::lock_guard<std::mutex> lk(state->mtx);
        bars = std::move(state->bars);
    }
    if (static_cast<int>(bars.size()) > n_bars) {
        bars.erase(bars.begin(), bars.begin() + (static_cast<int>(bars.size()) - n_bars));
    }
    return bars;
}

std::optional<double> broker::req_fx_rate(const std::string& base, const std::string& quote) {
    const std::string key = base + "/" + quote;
    {
        std::lock_guard<std::mutex> lk(fx_mtx_);
        const auto it = fx_cache_.find(key);
        if (it != fx_cache_.end() && std::chrono::steady_clock::now() - it->second.ts < fx_ttl) {
            return it->second.rate;
        }
    }

    auto state = std::make_shared<pending_hist>();
    int req_id = alloc_req_id();

    {
        std::lock_guard<std::mutex> lk(hist_map_mtx_);
        pending_hist_map_[req_id] = state;
    }

    client_->reqHistoricalData(req_id, make_forex_contract(base, quote), "", "3600 S", "1 min",
                               "MIDPOINT",
                               0, // forex trades 24 h
                               2, // epoch seconds — see ibkr_time_to_ns
                               false, TagValueListSPtr());

    {
        std::unique_lock<std::mutex> lk(state->mtx);
        bool ok =
            state->cv.wait_for(lk, std::chrono::seconds(10), [&state] { return state->done; });
        if (!ok) {
            LOG_WARNING(std::format("IBKR: req_fx_rate {}/{} timed out", base, quote));
        }
    }

    {
        std::lock_guard<std::mutex> lk(hist_map_mtx_);
        pending_hist_map_.erase(req_id);
    }

    double rate = 0.0;
    {
        std::lock_guard<std::mutex> lk(state->mtx);
        if (state->bars.empty()) {
            return std::nullopt;
        }
        rate = state->bars.back().close;
    }
    {
        std::lock_guard<std::mutex> lk(fx_mtx_);
        fx_cache_[key] = {rate, std::chrono::steady_clock::now()};
    }
    return rate;
}

std::optional<broker::account_info> broker::get_account_info() const {
    if (!connected_) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lk(account_mutex_);
    return cached_account_;
}

std::optional<std::vector<broker::position_info>> broker::get_positions() const {
    if (!connected_) {
        return std::nullopt; // unknown, NOT flat — caller must not erase tracked positions
    }
    std::lock_guard<std::mutex> lk(account_mutex_);
    return cached_positions_;
}

static Order base_order(const std::string& action, double qty) {
    Order o;
    o.action = action;
    o.totalQuantity = DecimalFunctions::doubleToDecimal(qty);
    o.tif = "DAY";
    o.transmit = true;
    return o;
}

namespace {
bool valid_order_id(int oid, const std::string& action, const std::string& symbol) {
    if (oid < 0) {
        LOG_ERROR(
            std::format("IBKR: no valid order id yet, refusing {} order for {}", action, symbol));
        return false;
    }
    return true;
}
} // namespace

int broker::place_market_order(const std::string& symbol, const std::string& action, double qty) {
    int oid = alloc_order_id();
    if (!valid_order_id(oid, action, symbol)) {
        return -1;
    }
    Order o = base_order(action, qty);
    o.orderType = "MKT";
    client_->placeOrder(oid, make_stock_contract(symbol), o);
    LOG_INFO(std::format("IBKR: MKT order {} {} {:.0f} {}", oid, action, qty, symbol));
    return oid;
}

int broker::place_limit_order(const std::string& symbol, const std::string& action, double qty,
                              double lmt) {
    int oid = alloc_order_id();
    if (!valid_order_id(oid, action, symbol)) {
        return -1;
    }
    Order o = base_order(action, qty);
    o.orderType = "LMT";
    o.lmtPrice = lmt;
    client_->placeOrder(oid, make_stock_contract(symbol), o);
    LOG_INFO(
        std::format("IBKR: LMT order {} {} {:.0f} {} @ {:.4f}", oid, action, qty, symbol, lmt));
    return oid;
}

int broker::place_stop_order(const std::string& symbol, const std::string& action, double qty,
                             double stop) {
    int oid = alloc_order_id();
    if (!valid_order_id(oid, action, symbol)) {
        return -1;
    }
    Order o = base_order(action, qty);
    o.orderType = "STP";
    o.auxPrice = stop;
    client_->placeOrder(oid, make_stock_contract(symbol), o);
    LOG_INFO(
        std::format("IBKR: STP order {} {} {:.0f} {} stop={:.4f}", oid, action, qty, symbol, stop));
    return oid;
}

int broker::place_stop_limit_order(const std::string& symbol, const std::string& action, double qty,
                                   double lmt, double stop) {
    int oid = alloc_order_id();
    if (!valid_order_id(oid, action, symbol)) {
        return -1;
    }
    Order o = base_order(action, qty);
    o.orderType = "STP LMT";
    o.lmtPrice = lmt;
    o.auxPrice = stop;
    client_->placeOrder(oid, make_stock_contract(symbol), o);
    LOG_INFO(std::format("IBKR: STP LMT order {} {} {:.0f} {} lmt={:.4f} stop={:.4f}", oid, action,
                         qty, symbol, lmt, stop));
    return oid;
}

int broker::place_trailing_stop(const std::string& symbol, const std::string& action, double qty,
                                double trail_pct) {
    int oid = alloc_order_id();
    if (!valid_order_id(oid, action, symbol)) {
        return -1;
    }
    Order o = base_order(action, qty);
    o.orderType = "TRAIL";
    o.trailingPercent = trail_pct;
    client_->placeOrder(oid, make_stock_contract(symbol), o);
    LOG_INFO(std::format("IBKR: TRAIL order {} {} {:.0f} {} trail={:.2f}%", oid, action, qty,
                         symbol, trail_pct));
    return oid;
}

broker::bracket_ids broker::place_bracket_order(const std::string& symbol,
                                                const std::string& action, double qty,
                                                double entry_lmt, double stop_price,
                                                double take_price, bool mkt_entry) {
    int parent_id = alloc_order_id();
    if (!valid_order_id(parent_id, action, symbol)) {
        return {.parent_id = -1, .stop_id = -1, .take_id = -1};
    }
    int stop_id = alloc_order_id();
    int take_id = alloc_order_id();

    const std::string opp = (action == "BUY") ? "SELL" : "BUY";
    auto c = make_stock_contract(symbol);
    auto dec = DecimalFunctions::doubleToDecimal(qty);

    const double entry_px = round_to_tick(entry_lmt);
    const double stop_px = round_to_tick(stop_price);
    const double take_px = round_to_tick(take_price);

    Order parent;
    parent.orderId = parent_id;
    parent.action = action;
    parent.totalQuantity = dec;
    parent.tif = "DAY";
    parent.transmit = false;
    if (mkt_entry) {
        parent.orderType = "MKT";
    } else {
        parent.orderType = "LMT";
        parent.lmtPrice = entry_px;
    }

    Order stop_ord;
    stop_ord.orderId = stop_id;
    stop_ord.action = opp;
    stop_ord.orderType = "STP";
    stop_ord.auxPrice = stop_px;
    stop_ord.totalQuantity = dec;
    stop_ord.parentId = parent_id;
    stop_ord.tif = "GTC";
    stop_ord.transmit = false;

    Order take_ord;
    take_ord.orderId = take_id;
    take_ord.action = opp;
    take_ord.orderType = "LMT";
    take_ord.lmtPrice = take_px;
    take_ord.totalQuantity = dec;
    take_ord.parentId = parent_id;
    take_ord.tif = "GTC";
    take_ord.transmit = true;

    client_->placeOrder(parent_id, c, parent);
    client_->placeOrder(stop_id, c, stop_ord);
    client_->placeOrder(take_id, c, take_ord);

    LOG_INFO(std::format("IBKR: bracket order parent={} stop={} take={} {} {:.0f} {} "
                         "entry={} sl={:.4f} tp={:.4f}",
                         parent_id, stop_id, take_id, action, qty, symbol,
                         mkt_entry ? "MKT" : std::format("LMT@{:.4f}", entry_px), stop_px,
                         take_px));

    return {.parent_id = parent_id, .stop_id = stop_id, .take_id = take_id};
}

bool broker::cancel_order(int order_id) {
    client_->cancelOrder(order_id, OrderCancel{});
    LOG_INFO(std::format("IBKR: cancel order {}", order_id));
    return true;
}

int broker::cancel_open_orders(const std::string& symbol) {
    std::vector<int> ids;
    {
        std::lock_guard<std::mutex> lk(live_orders_mtx_);
        for (const auto& [oid, sym] : live_orders_) {
            if (sym == symbol) {
                ids.push_back(oid);
            }
        }
    }
    for (const int oid : ids) {
        client_->cancelOrder(oid, OrderCancel{});
        LOG_INFO(std::format("IBKR: cancel open order {} ({})", oid, symbol));
    }
    return static_cast<int>(ids.size());
}

void broker::nextValidId(int orderId) {
    next_order_id_.store(orderId);
    LOG_DEBUG(std::format("IBKR: next valid order id = {}", orderId));

    std::lock_guard<std::mutex> lk(order_id_ready_.mtx);
    order_id_ready_.done = true;
    order_id_ready_.cv.notify_all();
}

void broker::managedAccounts(const std::string& accountsList) {
    {
        std::lock_guard<std::mutex> lk(account_mutex_);
        auto pos = accountsList.find(',');
        account_name_ = (pos != std::string::npos) ? accountsList.substr(0, pos) : accountsList;
        cached_account_.account = account_name_;
    }
    LOG_INFO(std::format("IBKR: managed account = {}", account_name_));

    std::lock_guard<std::mutex> lk(pending_connect_.mtx);
    pending_connect_.done = true;
    pending_connect_.cv.notify_all();
}

void broker::connectAck() {
    LOG_DEBUG("IBKR: connectAck");
    if (client_->asyncEConnect()) {
        client_->startApi();
    }
}

void broker::connectionClosed() {
    LOG_WARNING("IBKR: connection closed by server");
    connected_ = false;
}

void broker::error(int id, time_t, int errorCode, const std::string& errorString,
                   const std::string&) {
    if (errorCode == 1100) {
        LOG_ERROR("IBKR: TWS lost connectivity to IB servers (1100) — forcing reconnect");
        connected_ = false;
    }

    if (errorCode >= 2000 && errorCode < 3000) {
        LOG_INFO(std::format("IBKR info [{}] code={}: {}", id, errorCode, errorString));
    } else if (errorCode >= 1000) {
        LOG_WARNING(std::format("IBKR warning [{}] code={}: {}", id, errorCode, errorString));
    } else {
        LOG_ERROR(std::format("IBKR error [{}] code={}: {}", id, errorCode, errorString));
    }

    std::shared_ptr<pending_hist> state;
    {
        std::lock_guard<std::mutex> lk(hist_map_mtx_);
        auto it = pending_hist_map_.find(id);
        if (it != pending_hist_map_.end()) {
            state = it->second;
        }
    }
    if (state) {
        std::lock_guard<std::mutex> lk(state->mtx);
        state->done = true;
        state->cv.notify_all();
    }
}

void broker::winError(const std::string& str, int lastError) {
    LOG_ERROR(std::format("IBKR winError {}: {}", lastError, str));
}

void broker::historicalData(int reqId, const Bar& bar) {
    std::shared_ptr<pending_hist> state;
    {
        std::lock_guard<std::mutex> lk(hist_map_mtx_);
        auto it = pending_hist_map_.find(reqId);
        if (it == pending_hist_map_.end()) {
            return;
        }
        state = it->second;
    }
    auto iv = bar_to_interval(bar);
    if (iv.ts_ns < 0) {
        return;
    }
    if (!sanitize_interval(iv)) {
        LOG_WARNING(std::format("IBKR: malformed historical bar (req {}) o={} h={} l={} c={}, "
                                "dropping",
                                reqId, iv.open, iv.high, iv.low, iv.close));
        return;
    }
    std::lock_guard<std::mutex> lk(state->mtx);
    state->bars.push_back(iv);
}

void broker::historicalDataEnd(int reqId, const std::string&, const std::string&) {
    std::shared_ptr<pending_hist> state;
    {
        std::lock_guard<std::mutex> lk(hist_map_mtx_);
        auto it = pending_hist_map_.find(reqId);
        if (it == pending_hist_map_.end()) {
            return;
        }
        state = it->second;
    }
    {
        std::lock_guard<std::mutex> lk(state->mtx);
        state->done = true;
    }
    state->cv.notify_all();
}

void broker::historicalDataUpdate(int reqId, const Bar& bar) {
    historicalData(reqId, bar);
}

void broker::updateAccountValue(const std::string& key, const std::string& val,
                                const std::string& currency, const std::string&) {
    if (currency != "BASE" && currency != "SEK") {
        return;
    }

    try {
        double v = std::stod(val);
        std::lock_guard<std::mutex> lk(account_mutex_);
        if (key == "NetLiquidation") {
            cached_account_.net_liquidation = v;
        } else if (key == "BuyingPower") {
            cached_account_.buying_power = v;
        } else if (key == "AvailableFunds") {
            cached_account_.available_funds = v;
        } else if (key == "CashBalance") {
            cached_account_.cash_balance = v;
        } else if (key == "UnrealizedPnL") {
            cached_account_.unrealized_pnl = v;
        } else if (key == "RealizedPnL") {
            cached_account_.realized_pnl = v;
        }
    } catch (...) {}
}

void broker::updatePortfolio(const Contract& contract, Decimal position, double, double marketValue,
                             double averageCost, double unrealizedPNL, double, const std::string&) {
    double qty = DecimalFunctions::decimalToDouble(position);
    std::lock_guard<std::mutex> lk(account_mutex_);
    auto it = std::ranges::find_if(
        cached_positions_, [&](const position_info& p) { return p.symbol == contract.symbol; });
    if (it != cached_positions_.end()) {
        it->quantity = qty;
        it->avg_cost = averageCost;
        it->market_value = marketValue;
        it->unrealized_pnl = unrealizedPNL;
    } else if (qty != 0.0) {
        cached_positions_.push_back(
            {contract.symbol, qty, averageCost, marketValue, unrealizedPNL});
    }
    std::erase_if(cached_positions_, [](const position_info& p) { return p.quantity == 0.0; });
}

void broker::accountDownloadEnd(const std::string& accountName) {
    {
        std::lock_guard<std::mutex> lk(account_mutex_);
        cached_account_.account = accountName;
    }
    LOG_INFO(std::format("IBKR: account data ready for {}", accountName));

    std::lock_guard<std::mutex> lk(account_ready_.mtx);
    account_ready_.done = true;
    account_ready_.cv.notify_all();
}

void broker::execDetails(int, const Contract& contract, const Execution& execution) {
    int qty = static_cast<int>(DecimalFunctions::decimalToDouble(execution.shares));
    LOG_INFO(std::format("IBKR: fill order={} {} {} qty={} price={:.4f}", execution.orderId,
                         execution.side, contract.symbol, qty, execution.price));
    if (auto cb = order_callback()) {
        cb(execution.orderId, "Filled", execution.price, qty);
    }
}

void broker::orderStatus(int orderId, const std::string& status, Decimal filled, Decimal remaining,
                         double avgFillPrice, long long, int, double, int, const std::string&,
                         double) {
    const double rem = DecimalFunctions::decimalToDouble(remaining);
    LOG_INFO(std::format("IBKR: order {} status={} filled={:.0f} remaining={:.0f} avg={:.4f}",
                         orderId, status, DecimalFunctions::decimalToDouble(filled), rem,
                         avgFillPrice));

    if (status == "Cancelled" || status == "ApiCancelled" || status == "Inactive" ||
        (status == "Filled" && rem <= 0.0)) {
        std::lock_guard<std::mutex> lk(live_orders_mtx_);
        live_orders_.erase(orderId);
    }

    if (auto cb = order_callback()) {
        cb(orderId, status, avgFillPrice,
           static_cast<int>(DecimalFunctions::decimalToDouble(filled)));
    }
}

void broker::openOrder(int orderId, const Contract& c, const Order& o, const OrderState& s) {
    {
        std::lock_guard<std::mutex> lk(live_orders_mtx_);
        live_orders_[orderId] = c.symbol; // remember the working order so we can cancel it later
    }
    LOG_DEBUG(std::format("IBKR: open order {} {} {} {} status={}", orderId, c.symbol, o.action,
                          o.orderType, s.status));
}

void broker::cancel_real_time_bars(int id) {
    client_->cancelRealTimeBars(id);
    LOG_DEBUG(std::format("IBKR: canceled real-time bars id={}", id));
}

void broker::request_subscription(const std::string& symbol, int* req_id) {
    const int id = alloc_req_id();
    auto contract = make_stock_contract(symbol);

    client_->reqRealTimeBars(id, contract, 5, "TRADES", true, {});

    *req_id = id;
    LOG_DEBUG(std::format("IBKR: requested real-time bars id={} symbol={}", id, symbol));
}

void broker::realtimeBar(int id, long long time, double open, double high, double low, double close,
                         Decimal volume, Decimal open_interest, int contract_id) {
    LOG_DEBUG(std::format("IBKR: realtime bar id={} time={} open={} high={} low={} close={} "
                          "volume={} open_interest={} contract_id={}",
                          id, time, open, high, low, close,
                          DecimalFunctions::decimalToDouble(volume),
                          DecimalFunctions::decimalToDouble(open_interest), contract_id));

    market::interval iv{};
    iv.ts_ns = time * 1'000'000'000LL; // IBKR realtime bar time is in seconds since epoch
    iv.open = open;
    iv.high = high;
    iv.low = low;
    iv.close = close;
    iv.volume = static_cast<int64_t>(DecimalFunctions::decimalToDouble(volume));
    if (!sanitize_interval(iv)) {
        LOG_WARNING(std::format("IBKR: malformed realtime bar id={} o={} h={} l={} c={}, dropping",
                                id, open, high, low, close));
        return;
    }
    iv.datetime = market::to_ny(iv.ts_ns);

    orama::head->on_new_bar(iv, id);
}

void broker::tickPrice(int, TickType, double, const TickAttrib&) {}
void broker::tickSize(int, TickType, Decimal) {}
void broker::tickOptionComputation(int, TickType, int, double, double, double, double, double,
                                   double, double, double) {}
void broker::tickGeneric(int, TickType, double) {}
void broker::tickString(int, TickType, const std::string&) {}
void broker::tickEFP(int, TickType, double, const std::string&, double, int, const std::string&,
                     double, double) {}
void broker::openOrderEnd() {}
void broker::updateAccountTime(const std::string&) {}
void broker::contractDetails(int, const ContractDetails&) {}
void broker::bondContractDetails(int, const ContractDetails&) {}
void broker::contractDetailsEnd(int) {}
void broker::execDetailsEnd(int) {}
void broker::updateMktDepth(int, int, int, int, double, Decimal) {}
void broker::updateMktDepthL2(int, int, const std::string&, int, int, double, Decimal, bool) {}
void broker::updateNewsBulletin(int, int, const std::string&, const std::string&) {}
void broker::receiveFA(faDataType, const std::string&) {}
void broker::scannerParameters(const std::string&) {}
void broker::scannerData(int, int, const ContractDetails&, const std::string&, const std::string&,
                         const std::string&, const std::string&) {}
void broker::scannerDataEnd(int) {}
void broker::currentTime(long long) {}
void broker::fundamentalData(int, const std::string&) {}
void broker::deltaNeutralValidation(int, const DeltaNeutralContract&) {}
void broker::tickSnapshotEnd(int) {}
void broker::marketDataType(int, int) {}
void broker::commissionAndFeesReport(const CommissionAndFeesReport&) {}
void broker::accountSummary(int, const std::string&, const std::string&, const std::string&,
                            const std::string&) {}
void broker::accountSummaryEnd(int) {}
void broker::verifyMessageAPI(const std::string&) {}
void broker::verifyCompleted(bool, const std::string&) {}
void broker::displayGroupList(int, const std::string&) {}
void broker::displayGroupUpdated(int, const std::string&) {}
void broker::verifyAndAuthMessageAPI(const std::string&, const std::string&) {}
void broker::verifyAndAuthCompleted(bool, const std::string&) {}
void broker::positionMulti(int, const std::string&, const std::string&, const Contract&, Decimal,
                           double) {}
void broker::positionMultiEnd(int) {}
void broker::accountUpdateMulti(int, const std::string&, const std::string&, const std::string&,
                                const std::string&, const std::string&) {}
void broker::accountUpdateMultiEnd(int) {}
void broker::securityDefinitionOptionalParameter(int, const std::string&, int, const std::string&,
                                                 const std::string&, const std::set<std::string>&,
                                                 const std::set<double>&) {}
void broker::securityDefinitionOptionalParameterEnd(int) {}
void broker::softDollarTiers(int, const std::vector<SoftDollarTier>&) {}
void broker::familyCodes(const std::vector<FamilyCode>&) {}
void broker::symbolSamples(int, const std::vector<ContractDescription>&) {}
void broker::mktDepthExchanges(const std::vector<DepthMktDataDescription>&) {}
void broker::tickNews(int, time_t, const std::string&, const std::string&, const std::string&,
                      const std::string&) {}
void broker::smartComponents(int, const SmartComponentsMap&) {}
void broker::tickReqParams(int, double, const std::string&, int) {}
void broker::newsProviders(const std::vector<NewsProvider>&) {}
void broker::newsArticle(int, int, const std::string&) {}
void broker::historicalNews(int, const std::string&, const std::string&, const std::string&,
                            const std::string&) {}
void broker::historicalNewsEnd(int, bool) {}
void broker::headTimestamp(int, const std::string&) {}
void broker::histogramData(int, const HistogramDataVector&) {}
void broker::rerouteMktDataReq(int, int, const std::string&) {}
void broker::rerouteMktDepthReq(int, int, const std::string&) {}
void broker::marketRule(int, const std::vector<PriceIncrement>&) {}
void broker::pnl(int, double, double, double) {}
void broker::pnlSingle(int, Decimal, double, double, double, double) {}
void broker::historicalTicks(int, const std::vector<HistoricalTick>&, bool) {}
void broker::historicalTicksBidAsk(int, const std::vector<HistoricalTickBidAsk>&, bool) {}
void broker::historicalTicksLast(int, const std::vector<HistoricalTickLast>&, bool) {}
void broker::tickByTickAllLast(int, int, time_t, double, Decimal, const TickAttribLast&,
                               const std::string&, const std::string&) {}
void broker::tickByTickBidAsk(int, time_t, double, double, Decimal, Decimal,
                              const TickAttribBidAsk&) {}
void broker::tickByTickMidPoint(int, time_t, double) {}
void broker::orderBound(long long, int, int) {}
void broker::completedOrder(const Contract&, const Order&, const OrderState&) {}
void broker::completedOrdersEnd() {}
void broker::replaceFAEnd(int, const std::string&) {}
void broker::wshMetaData(int, const std::string&) {}
void broker::wshEventData(int, const std::string&) {}
void broker::historicalSchedule(int, const std::string&, const std::string&, const std::string&,
                                const std::vector<HistoricalSession>&) {}
void broker::userInfo(int, const std::string&) {}
void broker::currentTimeInMillis(time_t) {}
void broker::position(const std::string&, const Contract&, Decimal, double) {}
void broker::positionEnd() {}

// --- Protobuf stubs ---
#if !defined(USE_WIN_DLL)
void broker::execDetailsProtoBuf(const protobuf::ExecutionDetails&) {}
void broker::execDetailsEndProtoBuf(const protobuf::ExecutionDetailsEnd&) {}
void broker::orderStatusProtoBuf(const protobuf::OrderStatus&) {}
void broker::openOrderProtoBuf(const protobuf::OpenOrder&) {}
void broker::openOrdersEndProtoBuf(const protobuf::OpenOrdersEnd&) {}
void broker::errorProtoBuf(const protobuf::ErrorMessage&) {}
void broker::completedOrderProtoBuf(const protobuf::CompletedOrder&) {}
void broker::completedOrdersEndProtoBuf(const protobuf::CompletedOrdersEnd&) {}
void broker::orderBoundProtoBuf(const protobuf::OrderBound&) {}
void broker::contractDataProtoBuf(const protobuf::ContractData&) {}
void broker::bondContractDataProtoBuf(const protobuf::ContractData&) {}
void broker::contractDataEndProtoBuf(const protobuf::ContractDataEnd&) {}
void broker::tickPriceProtoBuf(const protobuf::TickPrice&) {}
void broker::tickSizeProtoBuf(const protobuf::TickSize&) {}
void broker::tickOptionComputationProtoBuf(const protobuf::TickOptionComputation&) {}
void broker::tickGenericProtoBuf(const protobuf::TickGeneric&) {}
void broker::tickStringProtoBuf(const protobuf::TickString&) {}
void broker::tickSnapshotEndProtoBuf(const protobuf::TickSnapshotEnd&) {}
void broker::updateMarketDepthProtoBuf(const protobuf::MarketDepth&) {}
void broker::updateMarketDepthL2ProtoBuf(const protobuf::MarketDepthL2&) {}
void broker::marketDataTypeProtoBuf(const protobuf::MarketDataType&) {}
void broker::tickReqParamsProtoBuf(const protobuf::TickReqParams&) {}
void broker::updateAccountValueProtoBuf(const protobuf::AccountValue&) {}
void broker::updatePortfolioProtoBuf(const protobuf::PortfolioValue&) {}
void broker::updateAccountTimeProtoBuf(const protobuf::AccountUpdateTime&) {}
void broker::accountDataEndProtoBuf(const protobuf::AccountDataEnd&) {}
void broker::managedAccountsProtoBuf(const protobuf::ManagedAccounts&) {}
void broker::positionProtoBuf(const protobuf::Position&) {}
void broker::positionEndProtoBuf(const protobuf::PositionEnd&) {}
void broker::accountSummaryProtoBuf(const protobuf::AccountSummary&) {}
void broker::accountSummaryEndProtoBuf(const protobuf::AccountSummaryEnd&) {}
void broker::positionMultiProtoBuf(const protobuf::PositionMulti&) {}
void broker::positionMultiEndProtoBuf(const protobuf::PositionMultiEnd&) {}
void broker::accountUpdateMultiProtoBuf(const protobuf::AccountUpdateMulti&) {}
void broker::accountUpdateMultiEndProtoBuf(const protobuf::AccountUpdateMultiEnd&) {}
void broker::historicalDataProtoBuf(const protobuf::HistoricalData&) {}
void broker::historicalDataUpdateProtoBuf(const protobuf::HistoricalDataUpdate&) {}
void broker::historicalDataEndProtoBuf(const protobuf::HistoricalDataEnd&) {}
void broker::realTimeBarTickProtoBuf(const protobuf::RealTimeBarTick&) {}
void broker::headTimestampProtoBuf(const protobuf::HeadTimestamp&) {}
void broker::histogramDataProtoBuf(const protobuf::HistogramData&) {}
void broker::historicalTicksProtoBuf(const protobuf::HistoricalTicks&) {}
void broker::historicalTicksBidAskProtoBuf(const protobuf::HistoricalTicksBidAsk&) {}
void broker::historicalTicksLastProtoBuf(const protobuf::HistoricalTicksLast&) {}
void broker::tickByTickDataProtoBuf(const protobuf::TickByTickData&) {}
void broker::updateNewsBulletinProtoBuf(const protobuf::NewsBulletin&) {}
void broker::newsArticleProtoBuf(const protobuf::NewsArticle&) {}
void broker::newsProvidersProtoBuf(const protobuf::NewsProviders&) {}
void broker::historicalNewsProtoBuf(const protobuf::HistoricalNews&) {}
void broker::historicalNewsEndProtoBuf(const protobuf::HistoricalNewsEnd&) {}
void broker::wshMetaDataProtoBuf(const protobuf::WshMetaData&) {}
void broker::wshEventDataProtoBuf(const protobuf::WshEventData&) {}
void broker::tickNewsProtoBuf(const protobuf::TickNews&) {}
void broker::scannerParametersProtoBuf(const protobuf::ScannerParameters&) {}
void broker::scannerDataProtoBuf(const protobuf::ScannerData&) {}
void broker::fundamentalsDataProtoBuf(const protobuf::FundamentalsData&) {}
void broker::pnlProtoBuf(const protobuf::PnL&) {}
void broker::pnlSingleProtoBuf(const protobuf::PnLSingle&) {}
void broker::receiveFAProtoBuf(const protobuf::ReceiveFA&) {}
void broker::replaceFAEndProtoBuf(const protobuf::ReplaceFAEnd&) {}
void broker::commissionAndFeesReportProtoBuf(const protobuf::CommissionAndFeesReport&) {}
void broker::historicalScheduleProtoBuf(const protobuf::HistoricalSchedule&) {}
void broker::rerouteMarketDataRequestProtoBuf(const protobuf::RerouteMarketDataRequest&) {}
void broker::rerouteMarketDepthRequestProtoBuf(const protobuf::RerouteMarketDepthRequest&) {}
void broker::secDefOptParameterProtoBuf(const protobuf::SecDefOptParameter&) {}
void broker::secDefOptParameterEndProtoBuf(const protobuf::SecDefOptParameterEnd&) {}
void broker::softDollarTiersProtoBuf(const protobuf::SoftDollarTiers&) {}
void broker::familyCodesProtoBuf(const protobuf::FamilyCodes&) {}
void broker::symbolSamplesProtoBuf(const protobuf::SymbolSamples&) {}
void broker::smartComponentsProtoBuf(const protobuf::SmartComponents&) {}
void broker::marketRuleProtoBuf(const protobuf::MarketRule&) {}
void broker::userInfoProtoBuf(const protobuf::UserInfo&) {}
void broker::nextValidIdProtoBuf(const protobuf::NextValidId&) {}
void broker::currentTimeProtoBuf(const protobuf::CurrentTime&) {}
void broker::currentTimeInMillisProtoBuf(const protobuf::CurrentTimeInMillis&) {}
void broker::verifyMessageApiProtoBuf(const protobuf::VerifyMessageApi&) {}
void broker::verifyCompletedProtoBuf(const protobuf::VerifyCompleted&) {}
void broker::displayGroupListProtoBuf(const protobuf::DisplayGroupList&) {}
void broker::displayGroupUpdatedProtoBuf(const protobuf::DisplayGroupUpdated&) {}
void broker::marketDepthExchangesProtoBuf(const protobuf::MarketDepthExchanges&) {}
void broker::configResponseProtoBuf(const protobuf::ConfigResponse&) {}
void broker::updateConfigResponseProtoBuf(const protobuf::UpdateConfigResponse&) {}
#endif

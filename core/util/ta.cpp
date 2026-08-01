#include "ta.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <optional>
#include <stdexcept>
#include <ta_common.h>
#include <ta_defs.h>
#include <ta_func.h>
#include <ta_libc.h>
#include <vector>

#include "../logs.hpp"
#include "../market.hpp"

namespace util::ta::templates {
template <typename Fn> std::vector<double> run(int n, Fn&& fn) {
    std::vector<double> raw(static_cast<size_t>(n));
    int outBeg{}, outNb{};
    fn(&outBeg, &outNb, raw.data());
    std::vector<double> out(static_cast<size_t>(n), std::numeric_limits<double>::quiet_NaN());
    for (int i = 0; i < outNb; ++i) {
        out[static_cast<size_t>(outBeg + i)] = raw[static_cast<size_t>(i)];
    }
    return out;
}

template <typename Fn> std::pair<std::vector<double>, std::vector<double>> run2(int n, Fn&& fn) {
    std::vector<double> rawA(static_cast<size_t>(n)), rawB(static_cast<size_t>(n));
    int outBeg{}, outNb{};
    fn(&outBeg, &outNb, rawA.data(), rawB.data());
    const double nan = std::numeric_limits<double>::quiet_NaN();
    std::vector<double> a(static_cast<size_t>(n), nan), b(static_cast<size_t>(n), nan);
    for (int i = 0; i < outNb; ++i) {
        a[static_cast<size_t>(outBeg + i)] = rawA[static_cast<size_t>(i)];
        b[static_cast<size_t>(outBeg + i)] = rawB[static_cast<size_t>(i)];
    }
    return {a, b};
}

template <typename Fn>
std::tuple<std::vector<double>, std::vector<double>, std::vector<double>> run3(int n, Fn&& fn) {
    std::vector<double> rawA(static_cast<size_t>(n)), rawB(static_cast<size_t>(n)),
        rawC(static_cast<size_t>(n));
    int outBeg{}, outNb{};
    fn(&outBeg, &outNb, rawA.data(), rawB.data(), rawC.data());
    const double nan = std::numeric_limits<double>::quiet_NaN();
    std::vector<double> a(static_cast<size_t>(n), nan), b(static_cast<size_t>(n), nan),
        c(static_cast<size_t>(n), nan);
    for (int i = 0; i < outNb; ++i) {
        a[static_cast<size_t>(outBeg + i)] = rawA[static_cast<size_t>(i)];
        b[static_cast<size_t>(outBeg + i)] = rawB[static_cast<size_t>(i)];
        c[static_cast<size_t>(outBeg + i)] = rawC[static_cast<size_t>(i)];
    }
    return {a, b, c};
}
} // namespace util::ta::templates

void util::ta::compute(std::vector<market::interval>& intervals) {
    const int n = static_cast<int>(intervals.size());
    if (n < lookback_period + 1) {
        return;
    }
    const auto un = static_cast<size_t>(n);

    std::vector<double> high(un), low(un), close(un), volume(un);
    for (size_t i = 0; i < un; ++i) {
        high[i] = intervals[i].high;
        low[i] = intervals[i].low;
        close[i] = intervals[i].close;
        volume[i] = static_cast<double>(intervals[i].volume);
    }

    auto ema = templates::run(n, [&](int* b, int* nb, double* o) {
        TA_EMA(0, n - 1, close.data(), ema_period, b, nb, o);
    });
    auto [aroon_dn, aroon_up] = templates::run2(n, [&](int* b, int* nb, double* a, double* u) {
        TA_AROON(0, n - 1, high.data(), low.data(), aroon_period, b, nb, a, u);
    });
    auto [bb_upper, bb_mid, bb_lower] =
        templates::run3(n, [&](int* b, int* nb, double* u, double* m, double* l) {
            TA_BBANDS(0, n - 1, close.data(), 20, 2.0, 2.0, TA_MAType_SMA, b, nb, u, m, l);
        });
    auto [macd, macd_sig, macd_hist] =
        templates::run3(n, [&](int* b, int* nb, double* m, double* s, double* h) {
            TA_MACD(0, n - 1, close.data(), 12, 26, 9, b, nb, m, s, h);
        });
    auto [mama, fama] = templates::run2(n, [&](int* b, int* nb, double* ma, double* fa) {
        TA_MAMA(0, n - 1, close.data(), mama_fast, mama_slow, b, nb, ma, fa);
    });
    auto rsi = templates::run(n, [&](int* b, int* nb, double* o) {
        TA_RSI(0, n - 1, close.data(), rsi_period, b, nb, o);
    });
    auto mfi = templates::run(n, [&](int* b, int* nb, double* o) {
        TA_MFI(0, n - 1, high.data(), low.data(), close.data(), volume.data(), mfi_period, b, nb,
               o);
    });

    using I = market::indicators;
    for (size_t i = 0; i < un; ++i) {
        auto set = [&](I ind, double val) {
            intervals[i].technicals[idx(ind)] = std::isnan(val)
                                                    ? std::optional<float>{}
                                                    : std::optional<float>{static_cast<float>(val)};
        };

        set(I::ema, ema[i]);
        set(I::aroon_down, aroon_dn[i]);
        set(I::aroon_up, aroon_up[i]);
        set(I::bollinger_upper, bb_upper[i]);
        set(I::bollinger_lower, bb_lower[i]);
        set(I::bollinger_mid, bb_mid[i]);
        set(I::macd, macd[i]);
        set(I::macd_signal, macd_sig[i]);
        set(I::macd_histogram, macd_hist[i]);
        set(I::mama, mama[i]);
        set(I::fama, fama[i]);
        set(I::rsi, rsi[i]);
        set(I::mfi, mfi[i]);
    }
}

void util::ta::drop_lookback(std::vector<market::interval>& intervals) {
    std::erase_if(intervals, [](const market::interval& b) {
        return std::ranges::any_of(b.technicals, [](const auto& t) { return !t.has_value(); });
    });
}

void util::ta::init() {
    TA_RetCode ret_code = TA_Initialize();
    if (ret_code != TA_SUCCESS) {
        throw std::runtime_error("TA-Lib initialization failed");
    }

    const int actual = std::max(
        {TA_MACD_Lookback(12, 26, 9), TA_MAMA_Lookback(mama_fast, mama_slow),
         TA_RSI_Lookback(rsi_period), TA_AROON_Lookback(aroon_period), TA_MFI_Lookback(mfi_period),
         TA_EMA_Lookback(ema_period), TA_BBANDS_Lookback(20, 2.0, 2.0, TA_MAType_SMA)});

    if (actual > lookback_period) {
        throw std::runtime_error(
            std::format("lookback_period ({}) too small; TA-Lib needs {} — update the constant",
                        lookback_period, actual));
    }
}

void util::ta::end() {
    TA_Shutdown();
}

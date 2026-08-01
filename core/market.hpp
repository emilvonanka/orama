#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include <date/date.h>
#include <date/tz.h>

namespace market {

struct ny_datetime {
    int year, month, day;
    int hour, minute;
};

inline ny_datetime to_ny(int64_t ts_ns) {
    static const date::time_zone* ny_tz = date::locate_zone("America/New_York");
    auto tp = std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{ts_ns}};
    auto zt = date::make_zoned(ny_tz, tp);
    auto lt = zt.get_local_time();

    auto dp = date::floor<date::days>(lt);
    date::year_month_day ymd{dp};
    date::hh_mm_ss hms{date::floor<std::chrono::minutes>(lt - dp)};

    return {
        .year = static_cast<int>(ymd.year()),
        .month = static_cast<int>(static_cast<unsigned>(ymd.month())),
        .day = static_cast<int>(static_cast<unsigned>(ymd.day())),
        .hour = static_cast<int>(hms.hours().count()),
        .minute = static_cast<int>(hms.minutes().count()),
    };
}

enum class indicators : std::uint8_t {
    ema,
    aroon_down,
    aroon_up,
    bollinger_upper,
    bollinger_lower,
    bollinger_mid,
    macd,
    macd_signal,
    macd_histogram,
    mama,
    fama,
    rsi,
    mfi,
    indicator_count
};

struct interval {
    int64_t ts_ns; // Timestamp in nanoseconds since the Unix epoch
    double open;
    double high;
    double low;
    double close;
    int64_t volume;

    std::array<std::optional<float>, static_cast<std::uint8_t>(indicators::indicator_count)>
        technicals;

    ny_datetime datetime;
};

inline const std::vector<std::string> equity_list = {
    "ADBE", "AMD",  "ABNB", "ALNY", "GOOGL", "GOOG",  "AMZN", "AEP",  "AMGN", "ADI",  "AAPL",
    "AMAT", "APP",  "ARM",  "ASML", "ADSK",  "ADP",   "AXON", "BKR",  "BKNG", "AVGO", "CDNS",
    "CHTR", "CTAS", "CSCO", "CCEP", "CTSH",  "CMCSA", "CEG",  "CPRT", "COST", "CRWD", "CSX",
    "DDOG", "DXCM", "FANG", "DASH", "EA",    "EXC",   "FAST", "FER",  "FTNT", "GEHC", "GILD",
    "HON",  "IDXX", "INSM", "INTC", "INTU",  "ISRG",  "KDP",  "KLAC", "KHC",  "LRCX", "LIN",
    "LITE", "MAR",  "MRVL", "MELI", "META",  "MCHP",  "MU",   "MSFT", "MSTR", "MDLZ", "MPWR",
    "MNST", "NFLX", "NVDA", "NXPI", "ORLY",  "ODFL",  "PCAR", "PLTR", "PANW", "PAYX", "PYPL",
    "PDD",  "PEP",  "QCOM", "REGN", "ROP",   "ROST",  "SNDK", "STX",  "SHOP", "SBUX", "SNPS",
    "TMUS", "TTWO", "TSLA", "TXN",  "TRI",   "VRSK",  "VRTX", "WMT",  "WBD",  "WDC",  "WDAY",
    "XEL",  "ZS",
};

} // namespace market
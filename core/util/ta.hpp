#pragma once
#include <algorithm>
#include <cstdint>
#include <tuple>
#include <vector>

#include "../market.hpp"

namespace util::ta {

inline constexpr int ema_period = 20;
inline constexpr int rsi_period = 14;
inline constexpr int aroon_period = 14;
inline constexpr int mfi_period = 14;
inline constexpr double mama_fast = 0.5;
inline constexpr double mama_slow = 0.05;

inline constexpr int lookback_period = 33;

constexpr std::uint8_t idx(market::indicators i) {
    return static_cast<std::uint8_t>(i);
}

void compute(std::vector<market::interval>& intervals);

void drop_lookback(std::vector<market::interval>& intervals);

void init();
void end();
} // namespace util::ta
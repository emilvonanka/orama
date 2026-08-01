#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>

namespace logs {
enum class level : std::uint8_t { debug, info, warning, error };

inline std::atomic<level> g_min_level{level::info};

inline void set_min_level(level lvl) noexcept {
    g_min_level.store(lvl, std::memory_order_relaxed);
}

[[nodiscard]] inline bool enabled(level lvl) noexcept {
    return static_cast<std::uint8_t>(lvl) >=
           static_cast<std::uint8_t>(g_min_level.load(std::memory_order_relaxed));
}

[[nodiscard]] inline std::string_view tag(level lvl) noexcept {
    switch (lvl) {
    case level::debug:
        return "[DEBUG]";
    case level::info:
        return "[INFO]";
    case level::warning:
        return "[WARNING]";
    case level::error:
        return "[ERROR]";
    }
    return "[?]";
}

inline void log(level lvl, std::string_view message) {
    if (!enabled(lvl)) {
        return;
    }

    const auto now =
        std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());

    std::string line = std::format("{:%Y-%m-%d %H:%M:%S} UTC {} {}\n", now, tag(lvl), message);

    static std::mutex io_mtx;
    std::lock_guard<std::mutex> lk(io_mtx);
    std::cout << line << std::flush;
}

inline void log_stats(const size_t buys, const size_t sells, const size_t holds, float highest_hold,
                      float highest_buy, float highest_sell, float total_hold, float total_buy,
                      float total_sell, size_t times_counted) {
    if (times_counted == 0) {
        return;
    }
    std::string line = std::format(
        "buys:{}, sells:{}, holds:{}, highest hold prob:{}, highest buy prob:{}, highest sell "
        "prob:{}, average hold prob:{}, average buy prob:{}, average sell prob:{}",
        buys, sells, holds, highest_hold, highest_buy, highest_sell,
        total_hold / static_cast<float>(times_counted),
        total_buy / static_cast<float>(times_counted),
        total_sell / static_cast<float>(times_counted));

    std::filesystem::path stats_path = std::filesystem::current_path() / "gen" / "stats.txt";
    std::filesystem::create_directories(stats_path.parent_path());

    std::ofstream f(stats_path, std::ios::trunc);
    if (f.is_open()) {

        f << line << "\n";
    } else {
        std::cerr << "CRITICAL: Could not open stats file at: " << stats_path << "\n";
    }
}
} // namespace logs

#define LOG_DEBUG(msg)                                                                             \
    do {                                                                                           \
        if (logs::enabled(logs::level::debug))                                                     \
            logs::log(logs::level::debug, (msg));                                                  \
    } while (0)
#define LOG_INFO(msg)                                                                              \
    do {                                                                                           \
        if (logs::enabled(logs::level::info))                                                      \
            logs::log(logs::level::info, (msg));                                                   \
    } while (0)
#define LOG_WARNING(msg)                                                                           \
    do {                                                                                           \
        if (logs::enabled(logs::level::warning))                                                   \
            logs::log(logs::level::warning, (msg));                                                \
    } while (0)
#define LOG_ERROR(msg)                                                                             \
    do {                                                                                           \
        if (logs::enabled(logs::level::error))                                                     \
            logs::log(logs::level::error, (msg));                                                  \
    } while (0)

#include "interface.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <implot.h>
#include <iterator>
#include <limits>
#include <optional>

#include "../core/util/ta.hpp"

namespace {
const char* to_string(model::model_action a) {
    switch (a) {
    case model::model_action::buy:
        return "buy";
    case model::model_action::sell:
        return "sell";
    case model::model_action::hold:
        return "hold";
    }
    return "?";
}

constexpr ImVec4 bull_color{0.20F, 0.75F, 0.35F, 1.0F};
constexpr ImVec4 bear_color{0.85F, 0.25F, 0.25F, 1.0F};
constexpr ImVec4 highlight_color{1.0F, 0.85F, 0.2F, 1.0F};

struct chart_overlay {
    std::optional<double> stop_price;
    std::optional<double> take_price;
    std::optional<size_t> highlight_idx; // index into the bars being drawn, e.g. the entry candle
};

double as_double(std::optional<float> v) {
    return v ? static_cast<double>(*v) : std::numeric_limits<double>::quiet_NaN();
}

void draw_candles(const std::vector<market::interval>& bars, const chart_overlay& overlay,
                  double half_width) {
    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    for (size_t i = 0; i < bars.size(); ++i) {
        const auto& bar = bars[i];
        double x = static_cast<double>(bar.ts_ns) / 1e9;
        bool bullish = bar.close >= bar.open;
        ImU32 color = ImGui::ColorConvertFloat4ToU32(bullish ? bull_color : bear_color);

        ImVec2 wick_top = ImPlot::PlotToPixels(x, bar.high);
        ImVec2 wick_bottom = ImPlot::PlotToPixels(x, bar.low);
        draw_list->AddLine(wick_top, wick_bottom, color, 1.0F);

        ImVec2 body_top = ImPlot::PlotToPixels(x - half_width, std::max(bar.open, bar.close));
        ImVec2 body_bottom = ImPlot::PlotToPixels(x + half_width, std::min(bar.open, bar.close));
        draw_list->AddRectFilled(body_top, body_bottom, color);

        if (overlay.highlight_idx && *overlay.highlight_idx == i) {
            ImU32 hl = ImGui::ColorConvertFloat4ToU32(highlight_color);
            draw_list->AddRect(body_top, body_bottom, hl, 0.0F, 0, 2.0F);
        }
    }
}

void plot_price_and_technicals(const char* id, const std::vector<market::interval>& bars,
                               const chart_overlay& overlay = {}) {
    if (bars.size() < 2) {
        ImGui::TextDisabled("warming up...");
        return;
    }

    const size_t n = bars.size();
    std::vector<double> xs(n);
    std::vector<double> ema(n);
    std::vector<double> boll_upper(n);
    std::vector<double> boll_mid(n);
    std::vector<double> boll_lower(n);
    std::vector<double> rsi(n);
    std::vector<double> macd(n);
    std::vector<double> macd_signal(n);
    std::vector<double> macd_hist(n);
    std::vector<double> mfi(n);
    std::vector<double> aroon_up(n);
    std::vector<double> aroon_down(n);

    double y_min = std::numeric_limits<double>::infinity();
    double y_max = -std::numeric_limits<double>::infinity();

    for (size_t i = 0; i < n; ++i) {
        const auto& bar = bars[i];
        xs[i] = static_cast<double>(bar.ts_ns) / 1e9;
        ema[i] = as_double(bar.technicals[util::ta::idx(market::indicators::ema)]);
        boll_upper[i] =
            as_double(bar.technicals[util::ta::idx(market::indicators::bollinger_upper)]);
        boll_mid[i] = as_double(bar.technicals[util::ta::idx(market::indicators::bollinger_mid)]);
        boll_lower[i] =
            as_double(bar.technicals[util::ta::idx(market::indicators::bollinger_lower)]);
        rsi[i] = as_double(bar.technicals[util::ta::idx(market::indicators::rsi)]);
        macd[i] = as_double(bar.technicals[util::ta::idx(market::indicators::macd)]);
        macd_signal[i] = as_double(bar.technicals[util::ta::idx(market::indicators::macd_signal)]);
        macd_hist[i] = as_double(bar.technicals[util::ta::idx(market::indicators::macd_histogram)]);
        mfi[i] = as_double(bar.technicals[util::ta::idx(market::indicators::mfi)]);
        aroon_up[i] = as_double(bar.technicals[util::ta::idx(market::indicators::aroon_up)]);
        aroon_down[i] = as_double(bar.technicals[util::ta::idx(market::indicators::aroon_down)]);

        y_min = std::min({y_min, bar.low, bar.high});
        y_max = std::max({y_max, bar.low, bar.high});
    }
    if (overlay.stop_price) {
        y_min = std::min(y_min, *overlay.stop_price);
        y_max = std::max(y_max, *overlay.stop_price);
    }
    if (overlay.take_price) {
        y_min = std::min(y_min, *overlay.take_price);
        y_max = std::max(y_max, *overlay.take_price);
    }

    double x_min = xs.front();
    double x_max = xs.back();

    double spacing = (xs.back() - xs.front()) / static_cast<double>(n - 1);
    double half_width = spacing * 0.35;

    std::array<float, 5> row_ratios{3.0F, 1.0F, 1.0F, 1.0F, 1.0F};
    if (ImPlot::BeginSubplots(id, 5, 1, ImVec2(-1, 480), ImPlotSubplotFlags_LinkAllX,
                              row_ratios.data())) {
        if (ImPlot::BeginPlot("##price")) {
            ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
            ImPlot::SetupAxesLimits(x_min, x_max, y_min, y_max, ImPlotCond_Once);
            draw_candles(bars, overlay, half_width);
            ImPlot::PlotLine("EMA", xs.data(), ema.data(), static_cast<int>(n));
            ImPlot::PlotLine("Boll Upper", xs.data(), boll_upper.data(), static_cast<int>(n));
            ImPlot::PlotLine("Boll Mid", xs.data(), boll_mid.data(), static_cast<int>(n));
            ImPlot::PlotLine("Boll Lower", xs.data(), boll_lower.data(), static_cast<int>(n));

            if (overlay.take_price) {
                double v = *overlay.take_price;
                ImPlot::PlotInfLines("Take Profit", &v, 1,
                                     ImPlotSpec(ImPlotProp_Flags, ImPlotInfLinesFlags_Horizontal,
                                                ImPlotProp_LineColor, bull_color));
            }
            if (overlay.stop_price) {
                double v = *overlay.stop_price;
                ImPlot::PlotInfLines("Stop Loss", &v, 1,
                                     ImPlotSpec(ImPlotProp_Flags, ImPlotInfLinesFlags_Horizontal,
                                                ImPlotProp_LineColor, bear_color));
            }
            ImPlot::EndPlot();
        }
        if (ImPlot::BeginPlot("##rsi")) {
            ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
            ImPlot::SetupAxisLimits(ImAxis_X1, x_min, x_max, ImPlotCond_Once);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 100.0, ImPlotCond_Always);
            ImPlot::PlotLine("RSI", xs.data(), rsi.data(), static_cast<int>(n));
            ImPlot::EndPlot();
        }
        if (ImPlot::BeginPlot("##macd")) {
            ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
            ImPlot::SetupAxisLimits(ImAxis_X1, x_min, x_max, ImPlotCond_Once);
            ImPlot::PlotBars("Histogram", xs.data(), macd_hist.data(), static_cast<int>(n),
                             spacing * 0.7);
            ImPlot::PlotLine("MACD", xs.data(), macd.data(), static_cast<int>(n));
            ImPlot::PlotLine("Signal", xs.data(), macd_signal.data(), static_cast<int>(n));
            ImPlot::EndPlot();
        }
        if (ImPlot::BeginPlot("##mfi")) {
            ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
            ImPlot::SetupAxisLimits(ImAxis_X1, x_min, x_max, ImPlotCond_Once);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 100.0, ImPlotCond_Always);
            ImPlot::PlotLine("MFI", xs.data(), mfi.data(), static_cast<int>(n));
            ImPlot::EndPlot();
        }
        if (ImPlot::BeginPlot("##aroon")) {
            ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
            ImPlot::SetupAxisLimits(ImAxis_X1, x_min, x_max, ImPlotCond_Once);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 100.0, ImPlotCond_Always);
            ImPlot::PlotLine("Aroon Up", xs.data(), aroon_up.data(), static_cast<int>(n));
            ImPlot::PlotLine("Aroon Down", xs.data(), aroon_down.data(), static_cast<int>(n));
            ImPlot::EndPlot();
        }
        ImPlot::EndSubplots();
    }
}

std::optional<size_t> find_entry_bar(const std::vector<market::interval>& bars,
                                     std::chrono::system_clock::time_point entry_time) {
    if (bars.empty()) {
        return std::nullopt;
    }
    int64_t entry_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(entry_time.time_since_epoch()).count();
    size_t best = 0;
    int64_t best_diff = std::abs(bars[0].ts_ns - entry_ns);
    for (size_t i = 1; i < bars.size(); ++i) {
        int64_t diff = std::abs(bars[i].ts_ns - entry_ns);
        if (diff < best_diff) {
            best_diff = diff;
            best = i;
        }
    }
    return best;
}
} // namespace

interface::interface() {
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    window_ = glfwCreateWindow(1280, 720, "orama", nullptr, nullptr);
    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init("#version 330");
}

void interface::end() {
    ImGui_ImplOpenGL3_Shutdown(); // 1. shutdown renderer backend
    ImGui_ImplGlfw_Shutdown();    // 2. shutdown platform backend
    ImGui::DestroyContext();      // 3. destroy imgui context
    ImPlot::DestroyContext();     // 4. destroy implot context
    glfwDestroyWindow(window_);   // 5. destroy window
    glfwTerminate();              // 6. terminate glfw
}

void interface::refresh_snapshot() {
    if (!orama::head) {
        return;
    }

    targets_.clear();
    for (const auto& t : orama::head->get_targets()) {
        targets_.emplace_back(t);

        auto& history = prob_history_[t.symbol];
        if (history.empty() || history.back().ts != t.last_updated) {
            history.push_back({t.last_updated, t.action_probs});
        }
        while (history.size() > max_prob_history) {
            history.pop_front();
        }
    }

    // drop history for symbols no longer targeted, so the map doesn't grow unbounded as targets
    // rotate in and out.
    for (auto it = prob_history_.begin(); it != prob_history_.end();) {
        bool still_targeted = std::any_of(targets_.begin(), targets_.end(),
                                          [&](const auto& t) { return t.symbol == it->first; });
        it = still_targeted ? std::next(it) : prob_history_.erase(it);
    }

    positions_.clear();
    for (const auto& p : orama::head->get_positions()) {
        positions_.emplace_back(p);
    }

    pending_orders_.clear();
    for (const auto& po : orama::head->get_pending_orders()) {
        pending_orders_.emplace_back(po);
    }

    entry_candidates_.clear();
    for (const auto& ec : orama::head->get_entry_candidates()) {
        entry_candidates_.emplace_back(ec);
    }

    stats_ = orama::head->get_stats();
}

void interface::render() {

    refresh_snapshot();

    // --- targets panel ---
    ImGui::SetNextWindowPos({0, 0}, ImGuiCond_Once);
    ImGui::SetNextWindowSize({700, 500}, ImGuiCond_Once);
    ImGui::Begin("Targets", nullptr);

    for (const auto& t : targets_) {
        ImGui::PushID(t.symbol.c_str());

        ImGui::Text("%s  conf=%.2f  action=%s", t.symbol.c_str(), static_cast<double>(t.confidence),
                    to_string(t.last_action));
        ImGui::Text("  probs: hold=%.2f buy=%.2f sell=%.2f", static_cast<double>(t.action_probs[0]),
                    static_cast<double>(t.action_probs[1]), static_cast<double>(t.action_probs[2]));

        if (auto it = prob_history_.find(t.symbol);
            it != prob_history_.end() && it->second.size() > 1) {
            const auto& history = it->second;
            std::vector<double> xs(history.size());
            std::vector<double> hold(history.size());
            std::vector<double> buy(history.size());
            std::vector<double> sell(history.size());
            for (size_t i = 0; i < history.size(); ++i) {
                xs[i] = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                history[i].ts.time_since_epoch())
                                                .count()) /
                        1e9;
                hold[i] = static_cast<double>(history[i].probs[0]);
                buy[i] = static_cast<double>(history[i].probs[1]);
                sell[i] = static_cast<double>(history[i].probs[2]);
            }

            if (ImPlot::BeginPlot("Action Probabilities", ImVec2(-1, 150))) {
                ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
                ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 1.0, ImPlotCond_Always);
                ImPlot::PlotLine("hold", xs.data(), hold.data(), static_cast<int>(xs.size()));
                ImPlot::PlotLine("buy", xs.data(), buy.data(), static_cast<int>(xs.size()));
                ImPlot::PlotLine("sell", xs.data(), sell.data(), static_cast<int>(xs.size()));
                ImPlot::EndPlot();
            }
        }

        plot_price_and_technicals("##price", t.window);

        ImGui::Separator();
        ImGui::PopID();
    }

    ImGui::End();

    // --- entry candidates panel ---
    ImGui::SetNextWindowPos({700, 200}, ImGuiCond_Once);
    ImGui::SetNextWindowSize({700, 300}, ImGuiCond_Once);
    ImGui::Begin("Entry Candidates", nullptr);

    for (const auto& ec : entry_candidates_) {
        ImGui::Text("%s  conf=%.2f  price=%.4f", ec.symbol.c_str(),
                    static_cast<double>(ec.confidence), ec.price);
    }
    ImGui::End();

    // --- pending orders panel ---
    ImGui::SetNextWindowPos({700, 500}, ImGuiCond_Once);
    ImGui::SetNextWindowSize({700, 400}, ImGuiCond_Once);
    ImGui::Begin("Pending Orders", nullptr);

    for (const auto& po : pending_orders_) {
        ImGui::Text("%s  qty=%zu  stop=%.4f  take=%.4f", po.symbol.c_str(), po.requested_qty,
                    po.stop_price, po.take_price);
    }
    ImGui::End();

    // --- positions panel ---
    ImGui::SetNextWindowPos({0, 500}, ImGuiCond_Once);
    ImGui::SetNextWindowSize({700, 400}, ImGuiCond_Once);
    ImGui::Begin("Positions", nullptr);

    for (const auto& p : positions_) {
        ImGui::PushID(p.symbol.c_str());

        ImGui::Text("%s  entry=%.4f  qty=%zu  unrealized=%.2f  conf=%.2f", p.symbol.c_str(),
                    p.entry_price, p.quantity, p.unrealized_pnl, static_cast<double>(p.confidence));

        chart_overlay overlay{
            .stop_price = p.stop_price,
            .take_price = p.take_price,
            .highlight_idx = find_entry_bar(p.window, p.entry_time),
        };
        plot_price_and_technicals("##price", p.window, overlay);

        ImGui::Separator();
        ImGui::PopID();
    }
    ImGui::End();

    // --- session stats ---
    ImGui::SetNextWindowPos({700, 0}, ImGuiCond_Once);
    ImGui::SetNextWindowSize({700, 200}, ImGuiCond_Once);
    ImGui::Begin("Session Stats", nullptr);

    ImGui::Text("buys:%zu, sells:%zu, holds:%zu", stats_.buys, stats_.sells, stats_.holds);
    ImGui::Text("highest hold prob:%.4f, highest buy prob:%.4f, highest sell prob:%.4f",
                static_cast<double>(stats_.highest_hold), static_cast<double>(stats_.highest_buy),
                static_cast<double>(stats_.highest_sell));
    ImGui::Text("average hold prob:%.4f, average buy prob:%.4f, average sell prob:%.4f",
                static_cast<double>(stats_.avg_hold), static_cast<double>(stats_.avg_buy),
                static_cast<double>(stats_.avg_sell));
    ImGui::Separator();
    ImGui::Text("net liquidation: %.2f  unrealized PnL: %.2f  realized PnL: %.2f",
                stats_.net_liquidation, stats_.unrealized_pnl, stats_.realized_pnl);

    ImGui::End();
}
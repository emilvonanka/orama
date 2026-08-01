#pragma once
#include <array>
#include <chrono>
#include <cstddef>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <GLFW/glfw3.h>

#include "../manager/manager.hpp"

class interface {
public:
    interface();
    void end();
    GLFWwindow* get_window() {
        return window_;
    }

    void render();

    struct orama_target : public manager::target {
        orama_target(const manager::target& t) : manager::target(t) {}
    };

    struct orama_position : public manager::position {
        orama_position(const manager::position& p) : manager::position(p) {}
        // @TODO: fill this
    };

    struct orama_pending_order : public manager::pending_order {
        orama_pending_order(const manager::pending_order& po) : manager::pending_order(po) {}
        // @TODO: fill this
    };

    struct orama_entry_candidate : public manager::entry_candidate {
        orama_entry_candidate(const manager::entry_candidate& ec) : manager::entry_candidate(ec) {}
        // @TODO: fill this
    };

private:
    void refresh_snapshot(); // pulls a thread safe snapshot from orama::head, once per frame

    struct prob_sample {
        std::chrono::system_clock::time_point ts;
        std::array<float, 3> probs; // hold, buy, sell
    };
    static constexpr size_t max_prob_history = 256;
    std::unordered_map<std::string, std::deque<prob_sample>> prob_history_;

    std::vector<orama_target> targets_;
    std::vector<orama_position> positions_;
    std::vector<orama_pending_order> pending_orders_;
    std::vector<orama_entry_candidate> entry_candidates_;
    manager::stats_snapshot stats_;
    GLFWwindow* window_;
};

inline std::unique_ptr<interface> ui = nullptr;
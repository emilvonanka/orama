#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <omp.h>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef USE_GUI
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include "../interface/interface.hpp"
#endif

#include "../manager/manager.hpp"
#include "../model/model.hpp"
#include "broker/broker.hpp"
#include "util/ta.hpp"

std::atomic<bool> g_wants_exit{false};
void signal_handler(int) {
    g_wants_exit = true;
}

void run_orama() {
    while (!g_wants_exit) {
        orama::head->cycle();

        // small sleep to avoid cpu unneccarily
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    orama::head->want_exit();
    orama::head->cycle();
}

int main(int /*argc*/, char* argv[]) {
    try {
        std::filesystem::current_path(std::filesystem::canonical(argv[0]).parent_path());
    } catch (const std::filesystem::filesystem_error&) {
        std::cerr << "warning: could not resolve executable path from argv[0], "
                     "using current directory for gen/ and raw_data/\n";
    }

    const std::vector<std::pair<std::string, std::string>> params = {
        // --- Core ---
        {"tree_method", "hist"},
        {"device", "cpu"}, // "cuda" if training on the 3060
        {"objective", "multi:softprob"},
        {"num_class", "3"},
        {"eval_metric", "mlogloss"},

        // --- Complexity ---
        {"max_depth", "4"},
        {"grow_policy", "depthwise"},
        {"min_child_weight", "200"},
        {"gamma", "0.5"},
        {"max_delta_step", "1"},

        // --- Regularisation ---
        {"lambda", "5.0"},
        {"alpha", "1.0"},

        // --- Sampling ---
        {"subsample", "0.7"},
        {"colsample_bytree", "0.6"},
        {"colsample_bylevel", "0.8"},
        {"colsample_bynode", "1.0"},

        // --- Learning ---
        {"learning_rate", "0.03"},
        {"max_bin", "256"},

        // --- Reproducibility / performance ---
        {"seed", "42"},
        // leave 2 cores for the rest of the process, but never go below 1 (a 1-2 core VPS
        // would otherwise pass 0/-1, which XGBoost interprets as "use everything")
        {"nthread", std::to_string(std::max(1, omp_get_max_threads() - 2))},
    };

    model::config conf{};
    // @TODO: make one confidece for entry and one for exit
    conf.minimum_confidence = 0.45f; // @TODO: hypertune
    conf.minimum_gain = 0.0025f;     // @TODO: hypertune
    conf.horizon = 10;               // @TODO: hypertune
    conf.window_size = 15;           // @TODO: hypertune
    conf.name = "orama_model";

    std::signal(SIGINT, signal_handler);

    orama::head = std::make_unique<manager>();
    util::ta::init();

    orama::head->create_model((conf), params);
    constexpr size_t max_target_count = 10;
    constexpr std::chrono::minutes target_tracking_time = std::chrono::minutes(90);

    orama::head->initialize(max_target_count, target_tracking_time);

#ifndef TRAIN_MODEL

#ifdef USE_GUI
    // launch backend in a separate thread so the GUI can run in the main thread (required by ImGui)
    std::thread backend_thread(run_orama);

    ui = std::make_unique<interface>();

    // render loop
    while (!glfwWindowShouldClose(ui->get_window())) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ui->render();

        ImGui::Render();

        int w, h;
        glfwGetFramebufferSize(ui->get_window(), &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(ui->get_window());
    }

    g_wants_exit = true;
    backend_thread.join();
    ui->end();

#endif

#ifndef USE_GUI
    while (orama::head->running()) {
        if (g_wants_exit) {
            orama::head->want_exit();
        }

        orama::head->cycle();

        // small sleep to avoid cpu unneccarily
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
#endif

#endif

    orama::head->end();
    util::ta::end();
    return 0;
}

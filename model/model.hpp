#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <xgboost/c_api.h>

#include "../core/logs.hpp"

class model {
public:
    enum class model_action : std::uint8_t { hold, buy, sell };

    struct input {
        std::vector<float> features;       // input features for the model
        std::optional<model_action> label; // optional label to take based on the model's prediction
        int64_t ts_ns; // timestamp of the input data, in nanoseconds since the Unix epoch, used for
                       // sorting
    };

    struct config {
        std::uint16_t horizon = 0U;     // number of steps to predict into the future
        std::uint16_t window_size = 0U; // number of past steps to use as input
        std::optional<float> minimum_gain = std::nullopt; // minimum gain required
        float minimum_confidence = 0.0F;                  // minimum confidence required
        std::string name;                                 // name of the model
    };

    model(config conf, const std::vector<std::pair<std::string, std::string>>& params)
        : params_(params), config_(std::move(conf)) {

        if (!std::filesystem::exists("gen")) {
            std::filesystem::create_directory("gen");
        }

        file_path_ = std::format("gen/{}.json", config_.name);

        initialize();
    }

    ~model() {
        end();
    }

    void save() {
        if (booster_ == nullptr) {
            LOG_ERROR("booster is not initialized, cannot save model");
            return;
        }

        if (!dirty_) {
            return;
        }

        const int ret = XGBoosterSaveModel(booster_, file_path_.c_str());
        if (ret != 0) {
            LOG_ERROR(std::format("couldn't save model: {}", XGBGetLastError()));
        } else {
            LOG_INFO(std::format("saved model"));
        }
    }

    [[nodiscard]] config get_config() const {
        return config_;
    }

    void learn(std::vector<input> inputs, int rounds) {
        if (inputs.empty()) {
            LOG_ERROR("no inputs");
            return;
        }
        dirty_ = true;

        const auto now = std::chrono::system_clock::now();
        const size_t rows = inputs.size();
        const size_t cols = inputs[0].features.size();

        std::vector<float> data, labels;
        data.reserve(rows * cols);
        labels.reserve(rows);

        for (auto& inp : inputs) {
            data.insert(data.end(), inp.features.begin(), inp.features.end());
            labels.push_back(static_cast<float>(static_cast<int>(*inp.label)));
            inp.features = {};
        }
        inputs = {};

        DMatrixHandle dtrain = nullptr;
        if (!xgb_ok(XGDMatrixCreateFromMat(data.data(), rows, cols, -999.0f, &dtrain),
                    "learn: create dtrain")) {
            return;
        }
        if (!xgb_ok(XGDMatrixSetFloatInfo(dtrain, "label", labels.data(), rows),
                    "learn: set labels")) {
            XGDMatrixFree(dtrain);
            return;
        }
        data = {};
        labels = {};

        set_parameters();

        XGBoosterSetParam(booster_, "num_feature", std::to_string(cols).c_str());

        const auto duration2 = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now() - now);
        LOG_INFO(std::format("learn: prepared {} rows in {} ms", rows, duration2.count()));

        const auto now3 = std::chrono::system_clock::now();
        for (int i = 0; i < rounds; ++i) {
            if (!xgb_ok(XGBoosterUpdateOneIter(booster_, i, dtrain), "learn: update")) {
                break;
            }
        }

        const auto duration3 = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now() - now3);
        LOG_INFO(std::format("learn: trained {} rounds in {} ms", rounds, duration3.count()));

        XGDMatrixFree(dtrain);

        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now() - now);
        LOG_INFO(
            std::format("batch took {} ms ({} rounds, {} rows)", duration.count(), rounds, rows));
    }

    // will learn how many rounds is optimal based on early stopping with a validation set
    void learn(std::vector<input> inputs) {
        if (inputs.empty()) {
            LOG_ERROR("no inputs");
            return;
        }
        dirty_ = true;

        const auto now = std::chrono::system_clock::now();

        // temporal split — last 20% as validation
        const auto split = static_cast<size_t>(static_cast<double>(inputs.size()) * 0.8);
        const size_t rows_train = split;
        const size_t rows_val = inputs.size() - split;
        const size_t cols = inputs[0].features.size();

        std::vector<float> train_data, train_labels;
        std::vector<float> val_data, val_labels;
        train_data.reserve(rows_train * cols);
        train_labels.reserve(rows_train);
        val_data.reserve(rows_val * cols);
        val_labels.reserve(rows_val);

        for (size_t i = 0; i < inputs.size(); ++i) {
            auto& inp = inputs[i];
            auto& target_data = i < split ? train_data : val_data;
            auto& target_labels = i < split ? train_labels : val_labels;
            target_data.insert(target_data.end(), inp.features.begin(), inp.features.end());
            target_labels.push_back(static_cast<float>(static_cast<int>(*inp.label)));
            inp.features = {};
        }
        inputs = {};

        DMatrixHandle dtrain = nullptr;
        DMatrixHandle dval = nullptr;
        if (!xgb_ok(XGDMatrixCreateFromMat(train_data.data(), rows_train, cols, -999.0f, &dtrain),
                    "learn: create dtrain")) {
            return;
        }
        xgb_ok(XGDMatrixSetFloatInfo(dtrain, "label", train_labels.data(), rows_train),
               "learn: set train labels");
        train_data = {};
        train_labels = {};

        if (!xgb_ok(XGDMatrixCreateFromMat(val_data.data(), rows_val, cols, -999.0f, &dval),
                    "learn: create dval")) {
            XGDMatrixFree(dtrain);
            return;
        }
        xgb_ok(XGDMatrixSetFloatInfo(dval, "label", val_labels.data(), rows_val),
               "learn: set val labels");
        val_data = {};
        val_labels = {};

        set_parameters();

        XGBoosterSetParam(booster_, "num_feature", std::to_string(cols).c_str());

        DMatrixHandle evals[] = {dtrain, dval};
        const char* eval_names[] = {"train", "val"};

        constexpr int max_rounds = 3500;
        constexpr int early_stopping_rounds = 50;
        double best_val_loss = std::numeric_limits<double>::max();
        int no_improve = 0;
        int best_round = 0;

        for (int i = 0; i < max_rounds; ++i) {
            if (!xgb_ok(XGBoosterUpdateOneIter(booster_, i, dtrain), "learn: update")) {
                break;
            }

            const char* eval_str = nullptr;
            XGBoosterEvalOneIter(booster_, i, evals, eval_names, 2, &eval_str);

            // parse val-mlogloss from "[i]\ttrain-mlogloss:X\tval-mlogloss:Y"
            double val_loss = parse_val_loss(eval_str);

            if (val_loss < best_val_loss - 1e-5) {
                best_val_loss = val_loss;
                best_round = i;
                no_improve = 0;
                XGBoosterSaveModel(booster_, file_path_.c_str()); // save best checkpoint
            } else {
                if (++no_improve >= early_stopping_rounds) {
                    LOG_INFO(
                        std::format("early stopping at round {} (best: round {}, val_loss: {:.5f})",
                                    i, best_round, best_val_loss));
                    break;
                }
            }
        }

        LOG_INFO(std::format("reloading best checkpoint (round {})", best_round));
        XGBoosterLoadModel(booster_, file_path_.c_str());

        XGDMatrixFree(dtrain);
        XGDMatrixFree(dval);

        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now() - now);
        LOG_INFO(std::format("model internal training took {} ms", duration.count()));
    }

    [[nodiscard]] std::array<float, 3> predict(const input& inp) const {
        if (booster_ == nullptr || inp.features.empty()) {
            return {1.0f, 0.0f, 0.0f};
        }

        const auto n_cols = static_cast<bst_ulong>(inp.features.size());
        DMatrixHandle dmat = nullptr;
        if (!xgb_ok(XGDMatrixCreateFromMat(inp.features.data(), 1, n_cols, -999.0f, &dmat),
                    "predict: create matrix")) {
            return {1.0f, 0.0f, 0.0f};
        }

        bst_ulong out_len = 0;
        const float* out_result = nullptr;
        const int pret = XGBoosterPredict(booster_, dmat, 0, 0, 0, &out_len, &out_result);
        XGDMatrixFree(dmat);

        if (!xgb_ok(pret, "predict") || out_len < 3 || out_result == nullptr) {
            return {1.0f, 0.0f, 0.0f};
        }

        return {out_result[0], out_result[1], out_result[2]};
    }

private:
    static bool xgb_ok(int ret, std::string_view what) {
        if (ret != 0) {
            LOG_ERROR(std::format("xgboost {}: {}", what, XGBGetLastError()));
            return false;
        }
        return true;
    }

    void initialize() {
        const auto ret = XGBoosterCreate(nullptr, 0, &booster_);
        if (ret != 0) {
            throw std::runtime_error("Failed to create XGBoost booster");
        }

        set_parameters();

        load();
    }

    void set_parameters() {
        for (const auto& [fst, snd] : params_) {
            const int ret = XGBoosterSetParam(booster_, fst.c_str(), snd.c_str());
            if (ret != 0) {
                LOG_ERROR(std::format("setting parameter {}: {}", fst, XGBGetLastError()));
            }
        }
    }

    void end() {
        save();
        XGBoosterFree(booster_);
    }

    void load() {
        if (!std::filesystem::exists(file_path_)) {
            return;
        }

        if (std::filesystem::is_empty(file_path_)) {
            return;
        }

        int ret = XGBoosterLoadModel(booster_, file_path_.c_str());

        if (ret != 0) {
            LOG_ERROR(std::format("couldn't load model: {}", XGBGetLastError()));
        } else {
            LOG_INFO(std::format("loaded model"));
        }
    }

    static double parse_val_loss(const char* eval_str) {
        if (eval_str == nullptr) {
            return std::numeric_limits<double>::max();
        }
        const std::string_view s(eval_str);
        constexpr std::string_view key = "val-mlogloss:";
        const auto pos = s.rfind(key);
        if (pos == std::string::npos) {
            return std::numeric_limits<double>::max();
        }
        // stod throws on a malformed eval string — that must degrade to "no improvement",
        // not kill a multi-hour training run.
        try {
            return std::stod(std::string(s.substr(pos + key.size())));
        } catch (const std::exception&) {
            return std::numeric_limits<double>::max();
        }
    }

    std::vector<std::pair<std::string, std::string>> params_;
    config config_;
    std::string file_path_;
    BoosterHandle booster_{};
    bool dirty_ = false; // true once this session trained; gates save() so inference never
                         // overwrites the on-disk model
};
#pragma once
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../core/market.hpp"
#include "../model/model.hpp"

class learner {
public:
    void start(std::unique_ptr<model>& orama);

    void set_config(const model::config& conf) {
        config_ = conf;
    }

    void once(); // this is a placeholder for later on live training

    std::optional<model::input>
    prepare_input(const std::vector<market::interval>& intervals,
                  std::optional<model::model_action> label = std::nullopt);

private:
    std::vector<model::input> compute_data(const std::string& path);
    std::vector<market::interval> get_data(const std::string& path);
    model::config config_;
};
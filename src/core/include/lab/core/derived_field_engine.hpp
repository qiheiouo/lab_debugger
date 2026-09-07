#pragma once

#include "lab/core/data_sample.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace lab::core {

struct DerivedFieldDefinition {
    std::string name;
    std::string expression;
    std::string unit;

    bool operator==(const DerivedFieldDefinition&) const = default;
};

struct DerivedFieldIssue {
    std::size_t definitionIndex{};
    std::string code;
    std::string message;
    std::size_t position{};
};

struct DerivedFieldConfigurationResult {
    std::vector<DerivedFieldIssue> issues;

    [[nodiscard]] bool success() const noexcept { return issues.empty(); }
};

class DerivedFieldEngine {
public:
    DerivedFieldEngine();
    ~DerivedFieldEngine();

    DerivedFieldEngine(const DerivedFieldEngine&) = delete;
    DerivedFieldEngine& operator=(const DerivedFieldEngine&) = delete;

    DerivedFieldConfigurationResult setDefinitions(
        std::vector<DerivedFieldDefinition> definitions);
    void clear();
    void resetValues();

    [[nodiscard]] std::vector<DerivedFieldDefinition> definitions() const;
    [[nodiscard]] std::vector<DataSample> consume(const DataSample& sample);
    [[nodiscard]] std::vector<DataSample> consumeBatch(
        std::span<const DataSample> samples);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lab::core

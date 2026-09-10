#pragma once

#include "lab/core/data_sample.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace lab::core {

enum class ThresholdComparison { Above, Below };

struct ThresholdAlertDefinition {
    std::string name;
    std::string field;
    ThresholdComparison comparison{ThresholdComparison::Above};
    double threshold{};
    double hysteresis{};
    std::string message;
    Timestamp durationNs{};

    bool operator==(const ThresholdAlertDefinition &) const = default;
};

struct ThresholdAlertIssue {
    std::size_t definitionIndex{};
    std::string code;
    std::string message;
};

struct ThresholdAlertConfigurationResult {
    std::vector<ThresholdAlertIssue> issues;

    [[nodiscard]] bool success() const noexcept {
        return issues.empty();
    }
};

struct ThresholdAlertTrigger {
    Timestamp timestamp{};
    std::string sourceId;
    std::string name;
    std::string field;
    ThresholdComparison comparison{ThresholdComparison::Above};
    double threshold{};
    double value{};
    std::string message;
    std::uint64_t sequence{};
    Timestamp durationNs{};
};

class ThresholdAlertEngine {
  public:
    ThresholdAlertEngine();
    ~ThresholdAlertEngine();

    ThresholdAlertEngine(const ThresholdAlertEngine &) = delete;
    ThresholdAlertEngine &operator=(const ThresholdAlertEngine &) = delete;

    ThresholdAlertConfigurationResult
    setDefinitions(std::vector<ThresholdAlertDefinition> definitions);
    void clear();
    void resetValues();

    [[nodiscard]] std::vector<ThresholdAlertDefinition> definitions() const;
    [[nodiscard]] std::vector<ThresholdAlertTrigger> consume(const DataSample &sample);
    [[nodiscard]] std::vector<ThresholdAlertTrigger>
    consumeBatch(std::span<const DataSample> samples);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string toString(ThresholdComparison comparison);

} // namespace lab::core

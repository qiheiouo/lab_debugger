#pragma once

#include "lab/core/timestamp.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace lab::core {

enum class HealthAlertKind { ErrorRate, Inactivity };

struct HealthAlertDefinition {
    std::string name;
    std::string sourceId;
    HealthAlertKind kind{HealthAlertKind::Inactivity};
    std::uint64_t errorCount{1};
    Timestamp windowNs{};
    std::string message;

    bool operator==(const HealthAlertDefinition&) const = default;
};

struct HealthAlertIssue {
    std::size_t definitionIndex{};
    std::string code;
    std::string message;
};

struct HealthAlertConfigurationResult {
    std::vector<HealthAlertIssue> issues;

    [[nodiscard]] bool success() const noexcept { return issues.empty(); }
};

struct HealthAlertTrigger {
    Timestamp timestamp{};
    std::string sourceId;
    std::string name;
    HealthAlertKind kind{HealthAlertKind::Inactivity};
    std::uint64_t observedErrors{};
    std::uint64_t configuredErrors{};
    Timestamp windowNs{};
    std::string message;
    std::uint64_t sequence{};
};

class HealthAlertEngine {
public:
    HealthAlertEngine();
    ~HealthAlertEngine();

    HealthAlertEngine(const HealthAlertEngine&) = delete;
    HealthAlertEngine& operator=(const HealthAlertEngine&) = delete;

    HealthAlertConfigurationResult
    setDefinitions(std::vector<HealthAlertDefinition> definitions);
    void clear();
    void resetValues();
    void arm(const std::string& sourceId, Timestamp timestamp);
    void disarm(const std::string& sourceId);
    void observeActivity(const std::string& sourceId, Timestamp timestamp);
    void observeError(const std::string& sourceId, Timestamp timestamp);

    [[nodiscard]] std::vector<HealthAlertDefinition> definitions() const;
    [[nodiscard]] std::vector<HealthAlertTrigger> evaluate(Timestamp timestamp);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string toString(HealthAlertKind kind);

}  // namespace lab::core

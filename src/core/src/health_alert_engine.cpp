#include "lab/core/health_alert_engine.hpp"

#include <algorithm>
#include <mutex>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace lab::core {
namespace {

constexpr std::size_t maximumDefinitions = 128;
constexpr std::size_t maximumNameLength = 256;
constexpr std::size_t maximumSourceIdLength = 1024;
constexpr std::size_t maximumMessageLength = 4096;
constexpr std::uint64_t maximumErrorCount = 10'000;
constexpr Timestamp minimumWindowNs = 1'000'000LL;
constexpr Timestamp maximumWindowNs = 86'400'000'000'000LL;

bool containsControlCharacter(std::string_view value) {
    return std::any_of(value.begin(), value.end(), [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte < 0x20U || byte == 0x7fU;
    });
}

bool validKind(HealthAlertKind kind) {
    return kind == HealthAlertKind::ErrorRate ||
           kind == HealthAlertKind::Inactivity;
}

}  // namespace

struct HealthAlertEngine::Impl {
    struct Rule {
        HealthAlertDefinition definition;
        bool active{};
        bool armed{};
        Timestamp lastActivity{};
        std::deque<Timestamp> errors;
    };

    mutable std::mutex mutex;
    std::vector<Rule> rules;
    std::uint64_t nextSequence{};
};

HealthAlertEngine::HealthAlertEngine() : impl_(std::make_unique<Impl>()) {}

HealthAlertEngine::~HealthAlertEngine() = default;

HealthAlertConfigurationResult
HealthAlertEngine::setDefinitions(
    std::vector<HealthAlertDefinition> definitions) {
    HealthAlertConfigurationResult result;
    if (definitions.size() > maximumDefinitions) {
        result.issues.push_back({maximumDefinitions,
                                 "too_many_definitions",
                                 "at most 128 health alert rules are allowed"});
        return result;
    }

    std::unordered_set<std::string> names;
    std::vector<Impl::Rule> rules;
    rules.reserve(definitions.size());
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        auto& definition = definitions[index];
        if (definition.name.empty() ||
            definition.name.size() > maximumNameLength ||
            containsControlCharacter(definition.name)) {
            result.issues.push_back(
                {index,
                 "invalid_name",
                 "rule name is empty, too long, or contains control characters"});
        } else if (!names.insert(definition.name).second) {
            result.issues.push_back(
                {index, "duplicate_name", "health alert rule names must be unique"});
        }
        if (definition.sourceId.empty() ||
            definition.sourceId.size() > maximumSourceIdLength ||
            containsControlCharacter(definition.sourceId)) {
            result.issues.push_back(
                {index,
                 "invalid_source_id",
                 "source id is empty, too long, or contains control characters"});
        }
        if (!validKind(definition.kind)) {
            result.issues.push_back(
                {index, "invalid_kind", "kind must be error_rate or inactivity"});
        }
        if (definition.errorCount == 0 ||
            definition.errorCount > maximumErrorCount) {
            result.issues.push_back(
                {index,
                 "invalid_error_count",
                 "error count must be between 1 and 10000"});
        }
        if (definition.windowNs < minimumWindowNs ||
            definition.windowNs > maximumWindowNs) {
            result.issues.push_back(
                {index,
                 "invalid_window",
                 "window must be between 1 millisecond and 24 hours"});
        }
        if (definition.message.size() > maximumMessageLength ||
            containsControlCharacter(definition.message)) {
            result.issues.push_back(
                {index,
                 "invalid_message",
                 "message is too long or contains control characters"});
        }
        rules.push_back(
            {std::move(definition), false, false, 0, {}});
    }
    if (!result.success()) return result;

    std::scoped_lock lock(impl_->mutex);
    impl_->rules = std::move(rules);
    impl_->nextSequence = 0;
    return result;
}

void HealthAlertEngine::clear() {
    std::scoped_lock lock(impl_->mutex);
    impl_->rules.clear();
    impl_->nextSequence = 0;
}

void HealthAlertEngine::resetValues() {
    std::scoped_lock lock(impl_->mutex);
    for (auto& rule : impl_->rules) {
        rule.active = false;
        rule.armed = false;
        rule.lastActivity = 0;
        rule.errors.clear();
    }
    impl_->nextSequence = 0;
}

void HealthAlertEngine::arm(
    const std::string& sourceId,
    Timestamp timestamp) {
    if (sourceId.empty() || timestamp <= 0) return;
    std::scoped_lock lock(impl_->mutex);
    for (auto& rule : impl_->rules) {
        if (rule.definition.sourceId != sourceId ||
            rule.definition.kind != HealthAlertKind::Inactivity) {
            continue;
        }
        rule.active = false;
        rule.armed = true;
        rule.lastActivity = timestamp;
    }
}

void HealthAlertEngine::disarm(const std::string& sourceId) {
    std::scoped_lock lock(impl_->mutex);
    for (auto& rule : impl_->rules) {
        if (rule.definition.sourceId != sourceId) continue;
        rule.active = false;
        rule.armed = false;
        rule.lastActivity = 0;
        rule.errors.clear();
    }
}

void HealthAlertEngine::observeActivity(
    const std::string& sourceId,
    Timestamp timestamp) {
    if (sourceId.empty() || timestamp <= 0) return;
    std::scoped_lock lock(impl_->mutex);
    for (auto& rule : impl_->rules) {
        if (rule.definition.sourceId != sourceId ||
            rule.definition.kind != HealthAlertKind::Inactivity) {
            continue;
        }
        if (rule.lastActivity != 0 && timestamp < rule.lastActivity) continue;
        rule.armed = true;
        rule.active = false;
        rule.lastActivity = timestamp;
    }
}

void HealthAlertEngine::observeError(
    const std::string& sourceId,
    Timestamp timestamp) {
    if (sourceId.empty() || timestamp <= 0) return;
    std::scoped_lock lock(impl_->mutex);
    for (auto& rule : impl_->rules) {
        if (rule.definition.sourceId != sourceId ||
            rule.definition.kind != HealthAlertKind::ErrorRate) {
            continue;
        }
        const auto position = std::upper_bound(
            rule.errors.begin(), rule.errors.end(), timestamp);
        rule.errors.insert(position, timestamp);
        while (rule.errors.size() > rule.definition.errorCount) {
            rule.errors.pop_front();
        }
    }
}

std::vector<HealthAlertDefinition> HealthAlertEngine::definitions() const {
    std::scoped_lock lock(impl_->mutex);
    std::vector<HealthAlertDefinition> result;
    result.reserve(impl_->rules.size());
    for (const auto& rule : impl_->rules) result.push_back(rule.definition);
    return result;
}

std::vector<HealthAlertTrigger>
HealthAlertEngine::evaluate(Timestamp timestamp) {
    if (timestamp <= 0) return {};
    std::scoped_lock lock(impl_->mutex);
    std::vector<HealthAlertTrigger> result;
    for (auto& rule : impl_->rules) {
        bool breached = false;
        std::uint64_t observedErrors = 0;
        if (rule.definition.kind == HealthAlertKind::ErrorRate) {
            while (!rule.errors.empty() &&
                   timestamp >= rule.errors.front() &&
                   timestamp - rule.errors.front() > rule.definition.windowNs) {
                rule.errors.pop_front();
            }
            observedErrors = static_cast<std::uint64_t>(rule.errors.size());
            breached = observedErrors >= rule.definition.errorCount;
        } else if (rule.armed && timestamp >= rule.lastActivity) {
            breached = timestamp - rule.lastActivity >= rule.definition.windowNs;
        }

        if (!breached) {
            rule.active = false;
            continue;
        }
        if (rule.active) continue;
        rule.active = true;
        result.push_back({timestamp,
                          rule.definition.sourceId,
                          rule.definition.name,
                          rule.definition.kind,
                          observedErrors,
                          rule.definition.errorCount,
                          rule.definition.windowNs,
                          rule.definition.message,
                          impl_->nextSequence++});
    }
    return result;
}

std::string toString(HealthAlertKind kind) {
    return kind == HealthAlertKind::ErrorRate ? "error_rate" : "inactivity";
}

}  // namespace lab::core

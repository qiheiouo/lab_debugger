#include "lab/core/threshold_alert_engine.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace lab::core {
namespace {

constexpr std::size_t maximumDefinitions = 128;
constexpr std::size_t maximumNameLength = 256;
constexpr std::size_t maximumFieldLength = 1024;
constexpr std::size_t maximumMessageLength = 4096;

bool containsControlCharacter(std::string_view value) {
    return std::any_of(value.begin(), value.end(), [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte < 0x20U || byte == 0x7fU;
    });
}

bool isValidComparison(ThresholdComparison comparison) {
    return comparison == ThresholdComparison::Above || comparison == ThresholdComparison::Below;
}

bool breached(const ThresholdAlertDefinition &definition, double value) {
    return definition.comparison == ThresholdComparison::Above ? value > definition.threshold
                                                               : value < definition.threshold;
}

bool recovered(const ThresholdAlertDefinition &definition, double value) {
    return definition.comparison == ThresholdComparison::Above
               ? value <= definition.threshold - definition.hysteresis
               : value >= definition.threshold + definition.hysteresis;
}

} // namespace

struct ThresholdAlertEngine::Impl {
    struct Rule {
        ThresholdAlertDefinition definition;
        bool active{};
    };

    mutable std::mutex mutex;
    std::vector<Rule> rules;
    std::uint64_t nextSequence{};
};

ThresholdAlertEngine::ThresholdAlertEngine() : impl_(std::make_unique<Impl>()) {}

ThresholdAlertEngine::~ThresholdAlertEngine() = default;

ThresholdAlertConfigurationResult
ThresholdAlertEngine::setDefinitions(std::vector<ThresholdAlertDefinition> definitions) {
    ThresholdAlertConfigurationResult result;
    if (definitions.size() > maximumDefinitions) {
        result.issues.push_back({maximumDefinitions, "too_many_definitions",
                                 "at most 128 threshold alert rules are allowed"});
        return result;
    }

    std::unordered_set<std::string> names;
    std::vector<Impl::Rule> rules;
    rules.reserve(definitions.size());
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        auto &definition = definitions[index];
        if (definition.name.empty() || definition.name.size() > maximumNameLength ||
            containsControlCharacter(definition.name)) {
            result.issues.push_back(
                {index, "invalid_name",
                 "rule name is empty, too long, or contains control characters"});
        } else if (!names.insert(definition.name).second) {
            result.issues.push_back(
                {index, "duplicate_name", "threshold alert rule names must be unique"});
        }
        if (definition.field.empty() || definition.field.size() > maximumFieldLength ||
            containsControlCharacter(definition.field)) {
            result.issues.push_back({index, "invalid_field",
                                     "field is empty, too long, or contains control characters"});
        }
        if (!isValidComparison(definition.comparison)) {
            result.issues.push_back(
                {index, "invalid_comparison", "comparison must be above or below"});
        }
        if (!std::isfinite(definition.threshold)) {
            result.issues.push_back({index, "invalid_threshold", "threshold must be finite"});
        }
        if (!std::isfinite(definition.hysteresis) || definition.hysteresis < 0.0) {
            result.issues.push_back(
                {index, "invalid_hysteresis", "hysteresis must be finite and non-negative"});
        }
        if (definition.message.size() > maximumMessageLength ||
            containsControlCharacter(definition.message)) {
            result.issues.push_back(
                {index, "invalid_message", "message is too long or contains control characters"});
        }
        rules.push_back({std::move(definition), false});
    }
    if (!result.success()) return result;

    std::scoped_lock lock(impl_->mutex);
    impl_->rules = std::move(rules);
    impl_->nextSequence = 0;
    return result;
}

void ThresholdAlertEngine::clear() {
    std::scoped_lock lock(impl_->mutex);
    impl_->rules.clear();
    impl_->nextSequence = 0;
}

void ThresholdAlertEngine::resetValues() {
    std::scoped_lock lock(impl_->mutex);
    for (auto &rule : impl_->rules)
        rule.active = false;
    impl_->nextSequence = 0;
}

std::vector<ThresholdAlertDefinition> ThresholdAlertEngine::definitions() const {
    std::scoped_lock lock(impl_->mutex);
    std::vector<ThresholdAlertDefinition> result;
    result.reserve(impl_->rules.size());
    for (const auto &rule : impl_->rules)
        result.push_back(rule.definition);
    return result;
}

std::vector<ThresholdAlertTrigger> ThresholdAlertEngine::consume(const DataSample &sample) {
    return consumeBatch(std::span(&sample, 1));
}

std::vector<ThresholdAlertTrigger>
ThresholdAlertEngine::consumeBatch(std::span<const DataSample> samples) {
    if (samples.empty()) return {};
    std::scoped_lock lock(impl_->mutex);
    std::vector<ThresholdAlertTrigger> result;
    for (const auto &sample : samples) {
        if (!std::isfinite(sample.value)) continue;
        for (auto &rule : impl_->rules) {
            if (sample.field != rule.definition.field) continue;
            if (rule.active) {
                if (recovered(rule.definition, sample.value)) rule.active = false;
                continue;
            }
            if (!breached(rule.definition, sample.value)) continue;
            rule.active = true;
            result.push_back({sample.timestamp, sample.sourceId, rule.definition.name,
                              rule.definition.field, rule.definition.comparison,
                              rule.definition.threshold, sample.value, rule.definition.message,
                              impl_->nextSequence++});
        }
    }
    return result;
}

std::string toString(ThresholdComparison comparison) {
    return comparison == ThresholdComparison::Above ? "above" : "below";
}

} // namespace lab::core

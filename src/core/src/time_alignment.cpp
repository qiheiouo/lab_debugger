#include "lab/core/time_alignment.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <string_view>

namespace lab::core {
namespace {

bool isDescendant(std::string_view sourceId, std::string_view configured) noexcept {
    return sourceId.size() > configured.size() &&
           sourceId.starts_with(configured) &&
           sourceId[configured.size()] == ':';
}

std::optional<Timestamp> checkedAdd(Timestamp value, Timestamp offset) noexcept {
    if (offset > 0 && value > std::numeric_limits<Timestamp>::max() - offset) {
        return std::nullopt;
    }
    if (offset < 0 && value < std::numeric_limits<Timestamp>::min() - offset) {
        return std::nullopt;
    }
    return value + offset;
}

std::optional<Timestamp> checkedSubtract(
    Timestamp left,
    Timestamp right) noexcept {
    if (right > 0 && left < std::numeric_limits<Timestamp>::min() + right) {
        return std::nullopt;
    }
    if (right < 0 && left > std::numeric_limits<Timestamp>::max() + right) {
        return std::nullopt;
    }
    return left - right;
}

bool containsControlCharacter(std::string_view value) noexcept {
    return std::ranges::any_of(value, [](unsigned char character) {
        return character < 0x20 || character == 0x7f;
    });
}

void saturatingIncrement(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) ++value;
}

}  // namespace

const char* toString(TimeAlignmentMode mode) noexcept {
    switch (mode) {
    case TimeAlignmentMode::SourceTime: return "source";
    case TimeAlignmentMode::ReceiveTime: return "receive";
    case TimeAlignmentMode::ManualOffset: return "manual";
    case TimeAlignmentMode::RemoteClock: return "remote_clock";
    }
    return "source";
}

std::optional<TimeAlignmentMode> timeAlignmentModeFromString(
    const std::string& value) noexcept {
    if (value == "source") return TimeAlignmentMode::SourceTime;
    if (value == "receive") return TimeAlignmentMode::ReceiveTime;
    if (value == "manual") return TimeAlignmentMode::ManualOffset;
    if (value == "remote_clock") return TimeAlignmentMode::RemoteClock;
    return std::nullopt;
}

TimeAlignmentConfigurationResult TimeAlignmentEngine::configure(
    std::vector<TimeAlignmentRule> rules) {
    TimeAlignmentConfigurationResult result;
    if (rules.size() > maximumRules) {
        result.issues.push_back({maximumRules, "time alignment has more than 64 rules"});
        return result;
    }

    std::set<std::string> sourceIds;
    for (std::size_t index = 0; index < rules.size(); ++index) {
        const auto& rule = rules[index];
        if (rule.sourceId.empty() || rule.sourceId.size() > 1024 ||
            containsControlCharacter(rule.sourceId)) {
            result.issues.push_back(
                {index, "source id is empty, too long, or contains control characters"});
        } else if (!sourceIds.insert(rule.sourceId).second) {
            result.issues.push_back({index, "source id is configured more than once"});
        }
        if (rule.mode == TimeAlignmentMode::ManualOffset && !rule.offsetNs) {
            result.issues.push_back({index, "manual alignment requires an offset"});
        }
        if (rule.mode != TimeAlignmentMode::SourceTime &&
            rule.mode != TimeAlignmentMode::ReceiveTime &&
            rule.mode != TimeAlignmentMode::ManualOffset &&
            rule.mode != TimeAlignmentMode::RemoteClock) {
            result.issues.push_back({index, "alignment mode is not supported"});
        }
        if (rule.offsetNs &&
            (*rule.offsetNs > maximumManualOffsetNs ||
             *rule.offsetNs < -maximumManualOffsetNs)) {
            result.issues.push_back({index, "offset is outside the seven day safety limit"});
        }
        if ((rule.mode == TimeAlignmentMode::SourceTime ||
             rule.mode == TimeAlignmentMode::ReceiveTime) && rule.offsetNs) {
            result.issues.push_back({index, "this alignment mode cannot carry an offset"});
        }
    }
    if (!result.success()) return result;

    std::ranges::sort(rules, {}, &TimeAlignmentRule::sourceId);
    std::scoped_lock lock(mutex_);
    rules_ = std::move(rules);
    clocks_.clear();
    measurements_.clear();
    return result;
}

std::vector<TimeAlignmentRule> TimeAlignmentEngine::rules() const {
    std::scoped_lock lock(mutex_);
    return rules_;
}

std::vector<TimeAlignmentRule> TimeAlignmentEngine::snapshotRules() const {
    std::scoped_lock lock(mutex_);
    auto result = rules_;
    for (auto& rule : result) {
        if (rule.mode != TimeAlignmentMode::RemoteClock) continue;
        if (const auto* estimate = matchingClock(rule.sourceId)) {
            const auto resolved = checkedSubtract(0, estimate->offsetNs);
            if (resolved) rule.offsetNs = *resolved;
        }
    }
    return result;
}

void TimeAlignmentEngine::updateRemoteClock(
    std::string sourceId,
    const ClockSyncEstimate& estimate) {
    if (sourceId.empty() || sourceId.size() > 1024 ||
        estimate.measuredAtNs <= 0 || estimate.roundTripNs < 0 ||
        estimate.uncertaintyNs < 0) {
        return;
    }
    std::scoped_lock lock(mutex_);
    clocks_.insert_or_assign(std::move(sourceId), estimate);
}

void TimeAlignmentEngine::clearRemoteClock(const std::string& sourceId) {
    std::scoped_lock lock(mutex_);
    clocks_.erase(sourceId);
}

TimeAlignmentResult TimeAlignmentEngine::align(
    const std::string& sourceId,
    Timestamp sourceTimestamp,
    Timestamp receiveTimestamp,
    bool observe) {
    std::scoped_lock lock(mutex_);
    const auto* rule = matchingRule(sourceId);
    const auto mode = rule ? rule->mode : TimeAlignmentMode::SourceTime;
    Timestamp timestamp = 0;
    Timestamp appliedOffset = 0;
    std::optional<Timestamp> uncertainty;
    bool fallback = false;

    const auto useSourceWithOffset = [&](Timestamp offset) {
        if (sourceTimestamp <= 0) return false;
        const auto aligned = checkedAdd(sourceTimestamp, offset);
        if (!aligned || *aligned <= 0) return false;
        timestamp = *aligned;
        appliedOffset = offset;
        return true;
    };

    switch (mode) {
    case TimeAlignmentMode::SourceTime:
        if (sourceTimestamp > 0) timestamp = sourceTimestamp;
        break;
    case TimeAlignmentMode::ReceiveTime:
        if (receiveTimestamp > 0) timestamp = receiveTimestamp;
        break;
    case TimeAlignmentMode::ManualOffset:
        if (rule && rule->offsetNs) {
            static_cast<void>(useSourceWithOffset(*rule->offsetNs));
        }
        break;
    case TimeAlignmentMode::RemoteClock: {
        const auto* clock = matchingClock(sourceId);
        if (clock) {
            const auto offset = checkedSubtract(0, clock->offsetNs);
            if (offset) static_cast<void>(useSourceWithOffset(*offset));
            uncertainty = clock->uncertaintyNs;
        } else if (rule && rule->offsetNs) {
            static_cast<void>(useSourceWithOffset(*rule->offsetNs));
        }
        break;
    }
    }

    if (timestamp <= 0) {
        fallback = true;
        timestamp = sourceTimestamp > 0 ? sourceTimestamp : receiveTimestamp;
        appliedOffset = 0;
    }
    if (timestamp <= 0) {
        timestamp = 0;
    }

    bool outOfOrder = false;
    if (observe && !sourceId.empty()) {
        auto& measurement = measurements_[sourceId];
        outOfOrder = measurement.lastAlignedTimestamp > 0 && timestamp > 0 &&
                     timestamp < measurement.lastAlignedTimestamp;
        saturatingIncrement(measurement.sampleCount);
        if (fallback) saturatingIncrement(measurement.fallbackCount);
        if (outOfOrder) saturatingIncrement(measurement.outOfOrderCount);
        if (timestamp > 0) measurement.lastAlignedTimestamp = timestamp;
        measurement.mode = mode;
        measurement.appliedOffsetNs = appliedOffset;
        measurement.uncertaintyNs = uncertainty;
        measurement.lastLatencyNs =
            timestamp > 0 && receiveTimestamp > 0
                ? checkedSubtract(receiveTimestamp, timestamp)
                : std::nullopt;
    }
    return {timestamp, appliedOffset, fallback, outOfOrder};
}

std::vector<TimeAlignmentStatus> TimeAlignmentEngine::statuses() const {
    std::scoped_lock lock(mutex_);
    std::vector<TimeAlignmentStatus> result;
    result.reserve(measurements_.size());
    for (const auto& [sourceId, measurement] : measurements_) {
        result.push_back({sourceId,
                          measurement.mode,
                          measurement.sampleCount,
                          measurement.fallbackCount,
                          measurement.outOfOrderCount,
                          measurement.appliedOffsetNs,
                          measurement.uncertaintyNs,
                          measurement.lastLatencyNs,
                          measurement.lastAlignedTimestamp});
    }
    std::ranges::sort(result, {}, &TimeAlignmentStatus::sourceId);
    return result;
}

void TimeAlignmentEngine::resetMeasurements() {
    std::scoped_lock lock(mutex_);
    measurements_.clear();
}

const TimeAlignmentRule* TimeAlignmentEngine::matchingRule(
    const std::string& sourceId) const noexcept {
    const TimeAlignmentRule* best = nullptr;
    for (const auto& rule : rules_) {
        if (sourceId != rule.sourceId && !isDescendant(sourceId, rule.sourceId)) {
            continue;
        }
        if (!best || rule.sourceId.size() > best->sourceId.size()) best = &rule;
    }
    return best;
}

const ClockSyncEstimate* TimeAlignmentEngine::matchingClock(
    const std::string& sourceId) const noexcept {
    const ClockSyncEstimate* best = nullptr;
    std::size_t bestLength = 0;
    for (const auto& [configured, estimate] : clocks_) {
        if (sourceId != configured && !isDescendant(sourceId, configured)) continue;
        if (!best || configured.size() > bestLength) {
            best = &estimate;
            bestLength = configured.size();
        }
    }
    return best;
}

}  // namespace lab::core

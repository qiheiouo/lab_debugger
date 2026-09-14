#pragma once

#include "lab/core/clock_sync.hpp"
#include "lab/core/timestamp.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lab::core {

enum class TimeAlignmentMode : std::uint8_t {
    SourceTime = 0,
    ReceiveTime = 1,
    ManualOffset = 2,
    RemoteClock = 3
};

[[nodiscard]] const char* toString(TimeAlignmentMode mode) noexcept;
[[nodiscard]] std::optional<TimeAlignmentMode> timeAlignmentModeFromString(
    const std::string& value) noexcept;

struct TimeAlignmentRule {
    std::string sourceId;
    TimeAlignmentMode mode{TimeAlignmentMode::SourceTime};
    // Added to the source timestamp. For RemoteClock this is a frozen
    // fallback captured when a Session starts.
    std::optional<Timestamp> offsetNs;

    bool operator==(const TimeAlignmentRule&) const = default;
};

struct TimeAlignmentIssue {
    std::size_t index{};
    std::string message;
};

struct TimeAlignmentConfigurationResult {
    std::vector<TimeAlignmentIssue> issues;

    [[nodiscard]] bool success() const noexcept { return issues.empty(); }
};

struct TimeAlignmentResult {
    Timestamp timestamp{};
    Timestamp appliedOffsetNs{};
    bool fallback{};
    bool outOfOrder{};
};

struct TimeAlignmentStatus {
    std::string sourceId;
    TimeAlignmentMode mode{TimeAlignmentMode::SourceTime};
    std::uint64_t sampleCount{};
    std::uint64_t fallbackCount{};
    std::uint64_t outOfOrderCount{};
    Timestamp appliedOffsetNs{};
    std::optional<Timestamp> uncertaintyNs;
    std::optional<Timestamp> lastLatencyNs;
    Timestamp lastAlignedTimestamp{};
};

class TimeAlignmentEngine final {
public:
    static constexpr std::size_t maximumRules = 64;
    static constexpr Timestamp maximumManualOffsetNs =
        7LL * 24 * 60 * 60 * 1'000'000'000;

    [[nodiscard]] TimeAlignmentConfigurationResult configure(
        std::vector<TimeAlignmentRule> rules);
    [[nodiscard]] std::vector<TimeAlignmentRule> rules() const;
    // Resolves current RemoteClock estimates into deterministic fallback
    // offsets suitable for a Session configuration snapshot.
    [[nodiscard]] std::vector<TimeAlignmentRule> snapshotRules() const;

    void updateRemoteClock(std::string sourceId,
                           const ClockSyncEstimate& estimate);
    void clearRemoteClock(const std::string& sourceId);

    [[nodiscard]] TimeAlignmentResult align(
        const std::string& sourceId,
        Timestamp sourceTimestamp,
        Timestamp receiveTimestamp,
        bool observe = true);
    [[nodiscard]] std::vector<TimeAlignmentStatus> statuses() const;
    void resetMeasurements();

private:
    struct Measurement {
        std::uint64_t sampleCount{};
        std::uint64_t fallbackCount{};
        std::uint64_t outOfOrderCount{};
        Timestamp lastAlignedTimestamp{};
        Timestamp appliedOffsetNs{};
        TimeAlignmentMode mode{TimeAlignmentMode::SourceTime};
        std::optional<Timestamp> uncertaintyNs;
        std::optional<Timestamp> lastLatencyNs;
    };

    [[nodiscard]] const TimeAlignmentRule* matchingRule(
        const std::string& sourceId) const noexcept;
    [[nodiscard]] const ClockSyncEstimate* matchingClock(
        const std::string& sourceId) const noexcept;

    mutable std::mutex mutex_;
    std::vector<TimeAlignmentRule> rules_;
    std::unordered_map<std::string, ClockSyncEstimate> clocks_;
    std::unordered_map<std::string, Measurement> measurements_;
};

}  // namespace lab::core

#pragma once

#include "lab/core/timestamp.hpp"

#include <cstddef>
#include <deque>
#include <optional>

namespace lab::core {

struct ClockSyncEstimate {
    Timestamp offsetNs{};
    Timestamp roundTripNs{};
    Timestamp uncertaintyNs{};
    Timestamp measuredAtNs{};
    std::size_t sampleCount{};

    bool operator==(const ClockSyncEstimate&) const = default;
};

class ClockSyncEstimator final {
public:
    explicit ClockSyncEstimator(
        std::size_t windowSize = 16,
        Timestamp maximumRoundTripNs = 10'000'000'000LL);

    [[nodiscard]] std::optional<ClockSyncEstimate> addSample(
        Timestamp localSendTimestamp,
        Timestamp agentTimestamp,
        Timestamp localReceiveTimestamp);
    [[nodiscard]] std::optional<ClockSyncEstimate> estimate() const;
    void reset() noexcept;

private:
    struct Sample {
        Timestamp offsetNs{};
        Timestamp roundTripNs{};
        Timestamp measuredAtNs{};
    };

    std::size_t windowSize_{};
    Timestamp maximumRoundTripNs_{};
    std::deque<Sample> samples_;
};

}  // namespace lab::core

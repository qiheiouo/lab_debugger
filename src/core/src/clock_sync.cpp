#include "lab/core/clock_sync.hpp"

#include <algorithm>
#include <stdexcept>

namespace lab::core {

ClockSyncEstimator::ClockSyncEstimator(
    std::size_t windowSize,
    Timestamp maximumRoundTripNs)
    : windowSize_(windowSize), maximumRoundTripNs_(maximumRoundTripNs) {
    if (windowSize_ == 0) {
        throw std::invalid_argument("Clock sync window must contain at least one sample");
    }
    if (maximumRoundTripNs_ <= 0) {
        throw std::invalid_argument("Clock sync maximum round trip must be positive");
    }
}

std::optional<ClockSyncEstimate> ClockSyncEstimator::addSample(
    Timestamp localSendTimestamp,
    Timestamp agentTimestamp,
    Timestamp localReceiveTimestamp) {
    if (localSendTimestamp <= 0 || agentTimestamp <= 0 ||
        localReceiveTimestamp < localSendTimestamp) {
        return std::nullopt;
    }

    const auto roundTrip = localReceiveTimestamp - localSendTimestamp;
    if (roundTrip > maximumRoundTripNs_) {
        return std::nullopt;
    }

    const auto midpoint = localSendTimestamp + roundTrip / 2;
    const auto offset = agentTimestamp >= midpoint
                            ? agentTimestamp - midpoint
                            : -(midpoint - agentTimestamp);
    samples_.push_back({offset, roundTrip, localReceiveTimestamp});
    while (samples_.size() > windowSize_) {
        samples_.pop_front();
    }
    return estimate();
}

std::optional<ClockSyncEstimate> ClockSyncEstimator::estimate() const {
    if (samples_.empty()) {
        return std::nullopt;
    }
    const auto best = std::min_element(
        samples_.begin(), samples_.end(), [](const Sample& left, const Sample& right) {
            return left.roundTripNs < right.roundTripNs;
        });
    return ClockSyncEstimate{
        best->offsetNs,
        best->roundTripNs,
        best->roundTripNs / 2,
        best->measuredAtNs,
        samples_.size()};
}

void ClockSyncEstimator::reset() noexcept {
    samples_.clear();
}

}  // namespace lab::core

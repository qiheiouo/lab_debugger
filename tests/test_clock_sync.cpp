#include "lab/core/clock_sync.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using lab::core::ClockSyncEstimator;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error("clock sync: " + message);
    }
}

void testConstructorValidation() {
    bool rejectedWindow = false;
    try {
        ClockSyncEstimator estimator(0);
    } catch (const std::invalid_argument&) {
        rejectedWindow = true;
    }
    require(rejectedWindow, "zero-sized windows are rejected");

    bool rejectedRoundTrip = false;
    try {
        ClockSyncEstimator estimator(4, 0);
    } catch (const std::invalid_argument&) {
        rejectedRoundTrip = true;
    }
    require(rejectedRoundTrip, "non-positive maximum round trips are rejected");
}

void testOffsetAndUncertainty() {
    ClockSyncEstimator estimator;
    const auto estimate = estimator.addSample(1'000, 1'550, 1'100);
    require(estimate.has_value(), "a valid sample produces an estimate");
    require(estimate->offsetNs == 500, "offset uses the local round-trip midpoint");
    require(estimate->roundTripNs == 100, "round trip is preserved");
    require(estimate->uncertaintyNs == 50, "uncertainty is half the round trip");
    require(estimate->measuredAtNs == 1'100 && estimate->sampleCount == 1,
            "estimate carries freshness and sample count");
}

void testMinimumRoundTripSampleWins() {
    ClockSyncEstimator estimator;
    static_cast<void>(estimator.addSample(1'000, 1'600, 1'200));
    const auto estimate = estimator.addSample(2'000, 2'540, 2'080);
    require(estimate && estimate->offsetNs == 500,
            "lowest-delay sample determines the offset");
    require(estimate->roundTripNs == 80 && estimate->sampleCount == 2,
            "best delay and complete window count are reported");
}

void testInvalidSamplesDoNotPolluteWindow() {
    ClockSyncEstimator estimator(4, 1'000);
    require(!estimator.addSample(0, 10, 20), "zero local timestamp is ignored");
    require(!estimator.addSample(20, 30, 10), "negative round trip is ignored");
    require(!estimator.addSample(10, 0, 20), "zero Agent timestamp is ignored");
    require(!estimator.addSample(10, 20, 1'011), "excessive round trip is ignored");
    require(!estimator.estimate(), "invalid samples leave the estimator empty");
}

void testRollingWindowAndReset() {
    ClockSyncEstimator estimator(2);
    static_cast<void>(estimator.addSample(1'000, 1'510, 1'020));
    static_cast<void>(estimator.addSample(2'000, 2'520, 2'040));
    const auto estimate = estimator.addSample(3'000, 3'550, 3'100);
    require(estimate && estimate->roundTripNs == 40 && estimate->sampleCount == 2,
            "oldest sample is evicted from the rolling window");
    require(estimate->offsetNs == 500,
            "remaining lowest-delay sample determines the estimate");
    estimator.reset();
    require(!estimator.estimate(), "reset removes all samples");
}

}  // namespace

int main() {
    try {
        testConstructorValidation();
        testOffsetAndUncertainty();
        testMinimumRoundTripSampleWins();
        testInvalidSamplesDoNotPolluteWindow();
        testRollingWindowAndReset();
        std::cout << "All clock synchronization tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Test failed: " << exception.what() << '\n';
        return 1;
    }
}

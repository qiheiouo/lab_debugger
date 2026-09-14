#include "lab/core/time_alignment.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using lab::core::TimeAlignmentEngine;
using lab::core::TimeAlignmentMode;
using lab::core::TimeAlignmentRule;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("time alignment: " + message);
}

void testConfigurationIsTransactional() {
    TimeAlignmentEngine engine;
    auto result = engine.configure({{"serial:COM5",
                                     TimeAlignmentMode::ManualOffset,
                                     25'000'000}});
    require(result.success(), "valid manual rule is accepted");

    result = engine.configure({{"serial:COM5",
                                TimeAlignmentMode::ManualOffset,
                                std::nullopt},
                               {"serial:COM5",
                                TimeAlignmentMode::ReceiveTime,
                                std::nullopt}});
    require(!result.success(), "missing offset and duplicate source are rejected");
    const auto definitions = engine.rules();
    require(definitions.size() == 1 &&
                definitions.front().offsetNs == 25'000'000,
            "invalid update does not replace active configuration");

    std::vector<TimeAlignmentRule> tooMany(TimeAlignmentEngine::maximumRules + 1);
    for (std::size_t index = 0; index < tooMany.size(); ++index) {
        tooMany[index].sourceId = "source:" + std::to_string(index);
    }
    require(!engine.configure(std::move(tooMany)).success(),
            "more than 64 rules are rejected");
    require(!engine.configure({{"bad\nsource",
                                TimeAlignmentMode::SourceTime,
                                std::nullopt}}).success(),
            "source ids containing control characters are rejected");
}

void testModesAndOriginalClockFallback() {
    TimeAlignmentEngine engine;
    require(engine.configure({
                {"serial:COM5", TimeAlignmentMode::ManualOffset, 20},
                {"udp:one", TimeAlignmentMode::ReceiveTime, std::nullopt},
                {"raw", TimeAlignmentMode::SourceTime, std::nullopt}})
                .success(),
            "mode configuration succeeds");

    const auto manual = engine.align("serial:COM5", 1'000, 1'100);
    require(manual.timestamp == 1'020 && manual.appliedOffsetNs == 20 &&
                !manual.fallback,
            "manual offset is added to source time");
    const auto receive = engine.align("udp:one", 2'000, 2'125);
    require(receive.timestamp == 2'125 && !receive.fallback,
            "receive mode uses desktop receive time");
    const auto raw = engine.align("raw", 3'000, 3'500);
    require(raw.timestamp == 3'000 && !raw.fallback,
            "source mode preserves original source time");
    const auto fallback = engine.align("raw", 0, 4'000);
    require(fallback.timestamp == 4'000 && fallback.fallback,
            "missing source time falls back to receive time");
}

void testRemoteClockInheritanceAndSnapshot() {
    TimeAlignmentEngine engine;
    require(engine.configure({{"ros-agent:robot-a",
                               TimeAlignmentMode::RemoteClock,
                               std::nullopt},
                              {"ros-agent:robot-a:/camera",
                               TimeAlignmentMode::ManualOffset,
                               7}})
                .success(),
            "remote rules are accepted");

    const auto beforeClock = engine.align(
        "ros-agent:robot-a:/imu", 10'000, 10'100);
    require(beforeClock.timestamp == 10'000 && beforeClock.fallback,
            "automatic mode preserves source time until clock is known");

    engine.updateRemoteClock(
        "ros-agent:robot-a",
        {.offsetNs = 500,
         .roundTripNs = 80,
         .uncertaintyNs = 40,
         .measuredAtNs = 9'000,
         .sampleCount = 3});
    const auto imu = engine.align("ros-agent:robot-a:/imu", 20'000, 19'600);
    require(imu.timestamp == 19'500 && imu.appliedOffsetNs == -500 &&
                !imu.fallback,
            "Agent minus desktop offset is subtracted from topic source time");
    const auto camera = engine.align(
        "ros-agent:robot-a:/camera", 20'000, 20'100);
    require(camera.timestamp == 20'007 && camera.appliedOffsetNs == 7,
            "longest matching topic rule overrides its Agent parent");

    const auto snapshot = engine.snapshotRules();
    require(snapshot.size() == 2, "snapshot preserves all rules");
    const auto automatic = std::ranges::find_if(snapshot, [](const auto& rule) {
        return rule.sourceId == "ros-agent:robot-a";
    });
    require(automatic != snapshot.end() && automatic->offsetNs == -500,
            "Session snapshot freezes the effective remote correction");

    engine.clearRemoteClock("ros-agent:robot-a");
    require(engine.align("ros-agent:robot-a:/imu", 30'000, 30'100).fallback,
            "clearing a live estimate prevents stale automatic correction");

    engine.updateRemoteClock(
        "ros-agent:robot-a",
        {.offsetNs = 900,
         .roundTripNs = 40,
         .uncertaintyNs = 20,
         .measuredAtNs = 20'000,
         .sampleCount = 4});
    require(engine.configure(snapshot).success(),
            "reconfiguring the engine accepts a frozen snapshot");
    const auto reconfigured = engine.align(
        "ros-agent:robot-a:/imu", 30'000, 30'100);
    require(reconfigured.timestamp == 29'500 && !reconfigured.fallback,
            "configuration replacement clears stale live clock estimates");

    TimeAlignmentEngine replay;
    require(replay.configure(snapshot).success(), "frozen snapshot can be restored");
    const auto restored = replay.align(
        "ros-agent:robot-a:/imu", 30'000, 30'100);
    require(restored.timestamp == 29'500 && !restored.fallback,
            "frozen remote correction reproduces the Session timeline");
}

void testSafetyAndMeasurements() {
    TimeAlignmentEngine engine;
    require(engine.configure({{"source",
                               TimeAlignmentMode::ManualOffset,
                               100}}).success(),
            "measurement rule is accepted");
    static_cast<void>(engine.align("source", 1'000, 1'300));
    const auto outOfOrder = engine.align("source", 800, 1'400);
    require(outOfOrder.outOfOrder, "backward aligned timestamp is reported");

    const auto statuses = engine.statuses();
    require(statuses.size() == 1 && statuses.front().sampleCount == 2 &&
                statuses.front().outOfOrderCount == 1 &&
                statuses.front().lastLatencyNs == 500,
            "status exposes samples, ordering, and latest latency");

    engine.resetMeasurements();
    require(engine.statuses().empty(), "measurement reset preserves no stale status");
    require(engine.rules().size() == 1, "measurement reset preserves configuration");

    const auto overflow = engine.align(
        "source", std::numeric_limits<lab::core::Timestamp>::max() - 50, 9'000);
    require(overflow.fallback &&
                overflow.timestamp == std::numeric_limits<lab::core::Timestamp>::max() - 50,
            "overflow never wraps and falls back to unchanged source time");
}

void testConcurrentObservation() {
    TimeAlignmentEngine engine;
    require(engine.configure({{"source",
                               TimeAlignmentMode::SourceTime,
                               std::nullopt}}).success(),
            "concurrency rule is accepted");
    std::vector<std::jthread> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&engine, worker] {
            for (int sample = 0; sample < 1000; ++sample) {
                const auto timestamp = 1'000'000 + worker * 10'000 + sample;
                static_cast<void>(engine.align("source", timestamp, timestamp + 10));
            }
        });
    }
    workers.clear();
    const auto statuses = engine.statuses();
    require(statuses.size() == 1 && statuses.front().sampleCount == 4000,
            "concurrent observations are serialized without loss");
}

}  // namespace

int main() {
    try {
        testConfigurationIsTransactional();
        testModesAndOriginalClockFallback();
        testRemoteClockInheritanceAndSnapshot();
        testSafetyAndMeasurements();
        testConcurrentObservation();
        std::cout << "All time alignment tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Test failed: " << exception.what() << '\n';
        return 1;
    }
}

#include "lab/core/csv_stream_parser.hpp"
#include "lab/core/mock_data_source.hpp"
#include "lab/core/processing_pipeline.hpp"
#include "lab/core/raw_log_recorder.hpp"
#include "lab/core/ring_buffer.hpp"
#include "lab/core/time_series_store.hpp"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void testRingBufferWrapAround() {
    lab::core::RingBuffer<int> buffer(3);
    buffer.push(1);
    buffer.push(2);
    buffer.push(3);
    buffer.push(4);
    require(buffer.snapshot() == std::vector<int>({2, 3, 4}), "ring order after wrap");
    buffer.clear();
    require(buffer.empty(), "ring clear");

    lab::core::RingBuffer<int> single(1);
    single.push(7);
    single.push(8);
    require(single.snapshot() == std::vector<int>({8}), "capacity one boundary");

    lab::core::RingBuffer<int> large(10'000);
    for (int value = 0; value < 100'000; ++value) {
        large.push(value);
    }
    const auto largeSnapshot = large.snapshot();
    require(largeSnapshot.front() == 90'000 && largeSnapshot.back() == 99'999,
            "large ring retains newest range");
}

void testTimeSeriesAndStatistics() {
    lab::core::TimeSeriesStore store(3);
    for (int value = 1; value <= 4; ++value) {
        store.append({value, "mock", "speed", static_cast<double>(value), "m/s", 0});
    }
    const auto points = store.snapshot("speed");
    require(points.size() == 3 && points.front().value == 2.0, "series wraps");
    const auto stats = store.statistics("speed");
    require(stats.has_value(), "stats exist");
    require(stats->minimum == 2.0 && stats->maximum == 4.0, "stats min/max");
    require(std::abs(stats->average - 3.0) < 1e-12, "stats average");

    lab::core::TimeSeriesStore analysisStore(100);
    analysisStore.append({0, "mock", "signal", 0.0, {}, 0});
    analysisStore.append({10, "mock", "signal", 100.0, {}, 0});
    analysisStore.append({20, "mock", "signal", 0.0, {}, 0});
    analysisStore.append({30, "mock", "signal", -50.0, {}, 0});
    analysisStore.append({40, "mock", "signal", 0.0, {}, 0});
    const auto reduced = analysisStore.downsampleMinMax("signal", 0, 4);
    require(std::any_of(reduced.begin(), reduced.end(), [](const auto& point) {
                return point.value == 100.0;
            }),
            "downsampling preserves positive peak");
    require(std::any_of(reduced.begin(), reduced.end(), [](const auto& point) {
                return point.value == -50.0;
            }),
            "downsampling preserves negative peak");
    const auto interpolated = analysisStore.interpolate("signal", 5);
    require(interpolated && std::abs(*interpolated - 50.0) < 1e-12,
            "linear interpolation");
}

void testCsvSplitChunksAndInvalidLine() {
    lab::core::CsvStreamParser parser({"speed", "current", "voltage"});
    const std::string first = "1.2,3";
    const std::string second = ".4,24.0\r\ninvalid,line\n5,6,7\n";
    auto samples = parser.consume(
        std::span(reinterpret_cast<const std::uint8_t*>(first.data()), first.size()),
        100,
        "mock",
        1);
    require(samples.empty(), "half line is buffered");
    samples = parser.consume(
        std::span(reinterpret_cast<const std::uint8_t*>(second.data()), second.size()),
        200,
        "mock",
        2);
    require(samples.size() == 6, "two valid lines parsed and invalid line rejected");
    require(samples[0].field == "speed" && samples[1].value == 3.4, "fields mapped");
}

void testMockDataSource() {
    lab::core::MockDataSource source;
    std::vector<lab::core::DataChunk> events;
    source.setCallbacks({[&events](const auto& chunk) { events.push_back(chunk); }, {}, {}});
    require(source.open(), "mock opens");
    const std::vector<std::uint8_t> rx{1, 2, 3};
    source.feed(rx, 42);
    const std::vector<std::uint8_t> tx{4, 5};
    require(source.write(tx), "mock writes");
    require(events.size() == 2, "mock publishes rx and tx");
    require(events[0].sourceTimestamp == 42, "source timestamp retained");
    require(source.statistics().receivedBytes == 3, "rx stats");
    source.close();
}

void testProcessingPipeline() {
    lab::core::TimeSeriesStore store(100);
    lab::core::ProcessingPipeline pipeline(store);
    pipeline.setFieldNames({"a", "b"});
    const std::string line = "10,20\n";
    pipeline.push({
        "mock",
        1'000,
        1'001,
        1,
        lab::core::Direction::Rx,
        std::vector<std::uint8_t>(line.begin(), line.end())});
    for (int attempt = 0; attempt < 100 && store.snapshot("a").empty(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    require(store.snapshot("a").size() == 1, "pipeline parses asynchronously");
    require(store.snapshot("b").front().value == 20.0, "pipeline stores values");
}

void testRawRecorder() {
    auto path = std::filesystem::temp_directory_path() / "lab_debugger_test.ldraw";
    lab::core::RawLogRecorder recorder;
    require(recorder.start(path), "recorder starts");
    recorder.enqueue({
        "mock", 1, 2, 3, lab::core::Direction::Rx, {0xAA, 0x55}});
    recorder.stop();
    require(std::filesystem::file_size(path) > 8, "recorder writes a record");
    std::filesystem::remove(path);
}

}  // namespace

int main() {
    try {
        testRingBufferWrapAround();
        testTimeSeriesAndStatistics();
        testCsvSplitChunksAndInvalidLine();
        testMockDataSource();
        testProcessingPipeline();
        testRawRecorder();
        std::cout << "All Lab Debugger core tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Test failed: " << exception.what() << '\n';
        return 1;
    }
}

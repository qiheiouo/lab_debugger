#include "lab/core/csv_stream_parser.hpp"
#include "lab/core/derived_field_engine.hpp"
#include "lab/core/mock_data_source.hpp"
#include "lab/core/processing_pipeline.hpp"
#include "lab/core/raw_log_reader.hpp"
#include "lab/core/raw_log_recorder.hpp"
#include "lab/core/replay_source.hpp"
#include "lab/core/ring_buffer.hpp"
#include "lab/core/session_recorder.hpp"
#include "lab/core/source_manager.hpp"
#include "lab/core/time_series_store.hpp"

#include <chrono>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <random>
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

std::filesystem::path uniqueTempPath(const std::string& stem) {
    std::random_device random;
    const auto token = (static_cast<std::uint64_t>(random()) << 32U) | random();
    return std::filesystem::temp_directory_path() /
           (stem + "-" + std::to_string(token));
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

void testDerivedFieldEngine() {
    lab::core::DerivedFieldEngine engine;
    const auto findOutput = [](const std::vector<lab::core::DataSample>& output,
                               const std::string& name) {
        return std::find_if(output.begin(), output.end(), [&name](const auto& sample) {
            return sample.field == name;
        });
    };
    const auto configured = engine.setDefinitions({
        {"power", "voltage * current", "W"},
        {"limited_power", "clamp(abs(power), 0, 1000)", "W"},
        {"source_error", "`serial:COM5.target-yaw` - `serial:COM5.yaw`", "rad"}});
    require(configured.success(), "safe derived expressions compile");

    require(engine.consume({10, "serial:COM5", "voltage", 24.0, "V", 1}).empty(),
            "derived output waits for every dependency");
    const auto power = engine.consume(
        {11, "serial:COM5", "current", 2.5, "A", 2});
    require(power.size() == 2 && power[0].field == "power" &&
                std::abs(power[0].value - 60.0) < 1e-12 &&
                power[0].sourceId == "derived" && power[0].unit == "W" &&
                power[1].field == "limited_power" && power[1].value == 60.0,
            "derived fields evaluate in dependency order");

    engine.resetValues();
    require(engine.consume(
                {14, "serial:COM5", "serial:COM5.voltage", 20.0, "V", 5}).empty(),
            "unique suffix shorthand waits for the second qualified input");
    const auto shorthand = engine.consume(
        {15, "serial:COM5", "serial:COM5.current", 3.0, "A", 6});
    require(shorthand.size() == 2 && shorthand.front().value == 60.0,
            "simple variables resolve against one unique qualified source");
    require(engine.consume(
                {16, "udp:127.0.0.1:9000", "udp:127.0.0.1:9000.voltage", 10.0, {}, 1})
                .empty(),
            "a second matching source makes shorthand ambiguous instead of mixing values");

    require(engine.consume({12, "serial:COM5", "target-yaw", 1.25, {}, 3}).empty(),
            "quoted source-qualified dependency waits for its pair");
    const auto error = engine.consume({13, "serial:COM5", "yaw", 0.5, {}, 4});
    require(error.size() == 1 && error.front().field == "source_error" &&
                std::abs(error.front().value - 0.75) < 1e-12,
            "backtick variables support punctuation in source-qualified names");

    const auto batchConfigured = engine.setDefinitions({
        {"difference", "left - right", {}},
        {"root", "sqrt(difference)", {}}});
    require(batchConfigured.success(), "batch expressions compile");
    const std::array<lab::core::DataSample, 2> batch{{
        {20, "mock", "left", 25.0, {}, 1},
        {20, "mock", "right", 9.0, {}, 1}}};
    const auto batchOutput = engine.consumeBatch(batch);
    require(batchOutput.size() == 2 && batchOutput[0].value == 16.0 &&
                batchOutput[1].value == 4.0,
            "a logical sample batch produces one consistent derived update");

    const auto divided = engine.setDefinitions({{"ratio", "numerator / denominator", {}}});
    require(divided.success(), "division expression compiles");
    static_cast<void>(engine.consume({30, "mock", "numerator", 4.0, {}, 1}));
    require(engine.consume({31, "mock", "denominator", 0.0, {}, 2}).empty(),
            "division by zero never creates a derived sample");
    require(engine.consume({32, "mock", "denominator", 2.0, {}, 3}).front().value == 2.0,
            "valid input recovers after a runtime domain error");
    require(engine.consume({33,
                            "mock",
                            "numerator",
                            std::numeric_limits<double>::quiet_NaN(),
                            {},
                            4}).empty(),
            "non-finite input invalidates dependent derived values");

    const auto math = engine.setDefinitions({
        {"arithmetic", "-(a + 2) * 3 / 2 - +b", {}},
        {"functions",
         "abs(c) + sqrt(d) + min(low, high) + max(low, high) + pow(base, 2) + "
         "clamp(limit, 0, 10)",
         {}},
        {"constants", "pi + e", {}}});
    require(math.success(), "operators, parentheses, functions, and constants compile");
    const std::array<lab::core::DataSample, 8> mathInputs{{
        {40, "mock", "a", 4.0, {}, 1},
        {40, "mock", "b", 1.0, {}, 1},
        {40, "mock", "c", -3.0, {}, 1},
        {40, "mock", "d", 16.0, {}, 1},
        {40, "mock", "low", 2.0, {}, 1},
        {40, "mock", "high", 5.0, {}, 1},
        {40, "mock", "base", 3.0, {}, 1},
        {40, "mock", "limit", 20.0, {}, 1}}};
    const auto mathOutput = engine.consumeBatch(mathInputs);
    const auto arithmetic = findOutput(mathOutput, "arithmetic");
    const auto functions = findOutput(mathOutput, "functions");
    const auto constants = findOutput(mathOutput, "constants");
    require(arithmetic != mathOutput.end() && arithmetic->value == -10.0,
            "four arithmetic operators, parentheses, and unary signs evaluate safely");
    require(functions != mathOutput.end() && functions->value == 33.0,
            "all whitelisted functions evaluate with the expected result");
    require(constants != mathOutput.end() &&
                std::abs(constants->value -
                         (3.14159265358979323846 + 2.71828182845904523536)) < 1e-12,
            "pi and e constants evaluate precisely");

    const auto runtimeErrors = engine.setDefinitions({
        {"negative_root", "sqrt(x)", {}},
        {"overflow", "huge * huge", {}},
        {"invalid_power", "pow(base_value, exponent)", {}}});
    require(runtimeErrors.success(), "runtime-domain test expressions compile");
    require(engine.consume({50, "mock", "x", -1.0, {}, 1}).empty(),
            "negative square root produces no sample");
    require(engine.consume({51,
                            "mock",
                            "huge",
                            std::numeric_limits<double>::max(),
                            {},
                            2}).empty(),
            "floating-point overflow produces no sample");
    static_cast<void>(engine.consume({52, "mock", "base_value", -1.0, {}, 3}));
    require(engine.consume({53, "mock", "exponent", 0.5, {}, 4}).empty(),
            "pow domain errors produce no sample");
    require(engine.consume({54,
                            "mock",
                            "x",
                            std::numeric_limits<double>::infinity(),
                            {},
                            5}).empty(),
            "infinite input produces no sample");
    const auto recoveredRoot = engine.consume({55, "mock", "x", 9.0, {}, 6});
    require(recoveredRoot.size() == 1 && recoveredRoot.front().value == 3.0,
            "valid input recovers after square-root and non-finite failures");

    const auto previous = engine.definitions();
    const auto cycle = engine.setDefinitions({{"a", "b + 1", {}}, {"b", "a + 1", {}}});
    require(!cycle.success() && cycle.issues.front().code == "dependency_cycle" &&
                engine.definitions() == previous,
            "invalid cyclic configuration does not replace the active configuration");
    const auto unsafe = engine.setDefinitions({{"bad", "system(1)", {}}});
    require(!unsafe.success() && unsafe.issues.front().code == "invalid_expression",
            "non-whitelisted functions are rejected");
    const auto duplicate =
        engine.setDefinitions({{"duplicate", "1", {}}, {"duplicate", "2", {}}});
    require(!duplicate.success() && duplicate.issues.front().code == "duplicate_name" &&
                engine.definitions() == previous,
            "duplicate names are rejected without replacing valid definitions");
    const auto tooLong =
        engine.setDefinitions({{"long", std::string(4097, '1'), {}}});
    require(!tooLong.success() &&
                tooLong.issues.front().code == "expression_too_long" &&
                engine.definitions() == previous,
            "overlong expressions are rejected transactionally");
    const auto nestedExpression = std::string(70, '(') + "1" + std::string(70, ')');
    const auto tooDeep = engine.setDefinitions({{"deep", nestedExpression, {}}});
    require(!tooDeep.success() && tooDeep.issues.front().code == "invalid_expression" &&
                engine.definitions() == previous,
            "overly deep expressions are rejected transactionally");
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
    source.setCallbacks(
        {[&events](const auto& chunk) { events.push_back(chunk); }, {}, {}, {}});
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

void testSourceManager() {
    lab::core::MockDataSource serial("serial:COM5");
    lab::core::MockDataSource network("udp:127.0.0.1:9000");
    lab::core::SourceManager manager;
    require(manager.add("serial", serial), "source manager accepts serial source");
    require(manager.add("network", network), "source manager accepts network source");
    require(!manager.add("serial", network), "source manager rejects duplicate stable key");
    require(!manager.add("serial-alias", serial),
            "source manager rejects registering one instance under multiple keys");

    std::vector<std::string> receivedKeys;
    std::vector<std::string> stateKeys;
    manager.setCallbacks({
        [&receivedKeys](const std::string& key, const lab::core::DataChunk&) {
            receivedKeys.push_back(key);
        },
        [&stateKeys](const std::string& key,
                     const std::string&,
                     lab::core::SourceState) { stateKeys.push_back(key); },
        {},
        {}});

    require(manager.open("serial") && manager.open("network"),
            "source manager opens multiple sources simultaneously");
    serial.feed(std::vector<std::uint8_t>{1, 2, 3}, 10);
    network.feed(std::vector<std::uint8_t>{4, 5}, 20);
    require(manager.write("network", std::vector<std::uint8_t>{6}),
            "source manager routes writes to the selected source");
    require(receivedKeys == std::vector<std::string>({"serial", "network", "network"}),
            "source manager preserves the stable source key for concurrent traffic");

    const auto statistics = manager.aggregateStatistics();
    require(statistics.receivedBytes == 5 && statistics.transmittedBytes == 1 &&
                statistics.receivedChunks == 2 && statistics.transmittedChunks == 1,
            "source manager aggregates traffic without losing per-source statistics");
    const auto sources = manager.sources();
    require(sources.size() == 2 && sources[0].key == "network" && sources[0].open &&
                sources[1].key == "serial" && sources[1].open,
            "source manager reports deterministic source snapshots");
    manager.closeAll();
    require(!serial.isOpen() && !network.isOpen() && stateKeys.size() == 8,
            "source manager closes every managed source and forwards state transitions");
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
    pipeline.flush();
    require(store.snapshot("a").size() == 1, "pipeline parses asynchronously");
    require(store.snapshot("b").front().value == 20.0, "pipeline stores values");
    pipeline.push({"mock", 2'000, 2'001, 2, lab::core::Direction::Tx, {1}});
    pipeline.flush();
    require(pipeline.pendingChunks() == 0, "pipeline flush handles non-RX chunks");
}

void testProcessingPipelineSeparatesSources() {
    lab::core::TimeSeriesStore store(100);
    lab::core::ProcessingPipeline pipeline(store);
    pipeline.setFieldNames({"left", "right"});
    pipeline.setQualifyFieldNames(true);

    const auto push = [&pipeline](std::string source,
                                  std::string bytes,
                                  lab::core::Timestamp timestamp,
                                  std::uint64_t sequence) {
        pipeline.push({std::move(source),
                       timestamp,
                       timestamp + 1,
                       sequence,
                       lab::core::Direction::Rx,
                       std::vector<std::uint8_t>(bytes.begin(), bytes.end())});
    };
    push("serial:COM5", "1,", 10, 1);
    push("udp:127.0.0.1:9000", "10,20\n", 20, 1);
    push("serial:COM5", "2\n", 30, 2);
    pipeline.flush();

    const auto serialLeft = store.snapshot("serial:COM5.left");
    const auto serialRight = store.snapshot("serial:COM5.right");
    const auto networkLeft = store.snapshot("udp:127.0.0.1:9000.left");
    const auto networkRight = store.snapshot("udp:127.0.0.1:9000.right");
    require(serialLeft.size() == 1 && serialRight.size() == 1 &&
                networkLeft.size() == 1 && networkRight.size() == 1,
            "interleaved sources produce independently named series");
    require(networkLeft.front().value == 10.0 && networkRight.front().value == 20.0 &&
                serialLeft.front().value == 1.0 && serialRight.front().value == 2.0,
            "partial rows never combine bytes from different sources");
}

void testRawRecorder() {
    const auto path = uniqueTempPath("lab_debugger_test") += ".ldraw";
    lab::core::RawLogRecorder recorder;
    require(recorder.start(path), "recorder starts");
    recorder.enqueue({
        "mock", 1, 2, 3, lab::core::Direction::Rx, {0xAA, 0x55}});
    recorder.stop();
    require(std::filesystem::file_size(path) > 8, "recorder writes a record");

    lab::core::RawLogReader reader;
    require(reader.open(path), "raw log reader opens recording");
    require(reader.recordCount() == 1, "raw log reader indexes records");
    const auto chunk = reader.read(0);
    require(chunk && chunk->sourceId == "mock" && chunk->payload == std::vector<std::uint8_t>({0xAA, 0x55}),
            "raw log reader preserves record content");
    reader.close();
    std::filesystem::remove(path);
}

void testTruncatedRawLogRecovery() {
    const auto path = uniqueTempPath("lab_debugger_truncated_test") += ".ldraw";
    lab::core::RawLogRecorder recorder;
    require(recorder.start(path), "truncated test recorder starts");
    recorder.enqueue({"mock", 10, 11, 1, lab::core::Direction::Rx, {1, 2, 3}});
    recorder.stop();
    {
        std::ofstream tail(path, std::ios::binary | std::ios::app);
        const std::array<std::uint8_t, 3> partial{0x52, 0x42, 0x44};
        tail.write(reinterpret_cast<const char*>(partial.data()),
                   static_cast<std::streamsize>(partial.size()));
    }

    lab::core::RawLogReader reader;
    require(reader.open(path), "reader accepts complete prefix before truncated tail");
    require(reader.recordCount() == 1 && reader.hasTruncatedTail(),
            "reader reports truncated tail without losing complete records");
    require(reader.read(0).has_value(), "recovered record remains readable");
    reader.close();

    {
        std::fstream corrupt(path, std::ios::binary | std::ios::in | std::ios::out);
        corrupt.seekp(8);
        const std::array<std::uint8_t, 4> invalidMarker{};
        corrupt.write(reinterpret_cast<const char*>(invalidMarker.data()),
                      static_cast<std::streamsize>(invalidMarker.size()));
    }
    require(!reader.open(path), "reader rejects a corrupted record marker");
    require(!reader.error().empty(), "corrupted raw log reports a useful error");
    std::filesystem::remove(path);
}

void testSessionRecorder() {
    const auto directory = uniqueTempPath("lab_debugger_session_test");
    std::error_code cleanupError;
    std::filesystem::remove_all(directory, cleanupError);

    lab::core::SessionRecorder recorder;
    lab::core::SessionStartOptions options;
    options.softwareVersion = "test";
    options.sessionName = "unit test";
    options.machineName = "test-host";
    options.operatingSystem = "test-os";
    options.protocolName = "demo";
    options.protocolJson = "{\"name\":\"demo\"}";
    options.csvFields = {"speed", "voltage"};
    options.derivedFields = {{"power", "voltage * current", "W"}};
    options.sources.push_back({"serial:COM1", "serial", "COM1", {{"baud", "115200"}}});
    require(recorder.start(directory, options), "session recorder starts");
    recorder.enqueueRaw({"serial:COM1", 100, 101, 1, lab::core::Direction::Rx, {0xAA}});
    recorder.enqueueSample({100, "serial:COM1", "voltage", 24.5, "V", 1});
    lab::core::FrameEvent frame;
    frame.sourceTimestamp = 100;
    frame.sourceId = "serial:COM1";
    frame.sequence = 1;
    frame.fields.push_back({"voltage", lab::core::FieldType::UInt16,
                            std::uint64_t{2450}, 24.5, "V", {}});
    recorder.enqueueFrame(std::move(frame));
    recorder.enqueueEvent({100, "serial:COM1", "warning", "checksum", "CRC mismatch", 1});
    recorder.stop();

    require(std::filesystem::exists(directory / "metadata.json"), "session metadata exists");
    require(std::filesystem::exists(directory / "raw" / "stream.ldraw"),
            "session raw stream exists");
    require(std::filesystem::exists(directory / "values.csv"), "session values exist");
    require(std::filesystem::exists(directory / "frames.jsonl"), "session frames exist");
    require(std::filesystem::exists(directory / "events.jsonl"), "session events exist");
    require(std::filesystem::exists(directory / "protocol" / "initial.json"),
            "session protocol snapshot exists");
    require(std::filesystem::exists(directory / "configuration" / "source_0.json"),
            "session source configuration exists");
    require(std::filesystem::exists(directory / "configuration" / "csv_fields.txt"),
            "session CSV field configuration exists");
    require(std::filesystem::exists(directory / "configuration" / "derived_fields.json"),
            "session derived field configuration exists");

    std::ifstream metadata(directory / "metadata.json");
    const std::string metadataText{
        std::istreambuf_iterator<char>(metadata), std::istreambuf_iterator<char>()};
    require(metadataText.find("\"status\": \"completed\"") != std::string::npos,
            "session metadata is finalized");
    require(metadataText.find("\"samples\": 1") != std::string::npos,
            "session metadata contains sample count");
    require(metadataText.find("\"frames\": 1") != std::string::npos,
            "session metadata contains frame count");
    metadata.close();

    std::ifstream values(directory / "values.csv");
    const std::string valuesText{
        std::istreambuf_iterator<char>(values), std::istreambuf_iterator<char>()};
    require(valuesText.find("voltage,24.5,V") != std::string::npos,
            "decoded sample is recorded");
    values.close();

    std::ifstream derived(directory / "configuration" / "derived_fields.json");
    const std::string derivedText{
        std::istreambuf_iterator<char>(derived), std::istreambuf_iterator<char>()};
    require(derivedText.find("\"expression\": \"voltage * current\"") !=
                std::string::npos,
            "session preserves safe derived expressions for deterministic replay");

    lab::core::SessionRecorder overwriteGuard;
    require(!overwriteGuard.start(directory, options),
            "session recorder refuses to overwrite a non-empty directory");

    std::filesystem::remove_all(directory, cleanupError);
}

void testReplaySourceControls() {
    const auto path = uniqueTempPath("lab_debugger_replay_test") += ".ldraw";
    lab::core::RawLogRecorder recorder;
    require(recorder.start(path), "replay fixture recorder starts");
    recorder.enqueue({"serial:COM1", 10, 1'000'000'000, 1, lab::core::Direction::Rx, {1}});
    recorder.enqueue({"serial:COM1", 20, 1'200'000'000, 2, lab::core::Direction::Rx, {2}});
    recorder.enqueue({"serial:COM1", 30, 1'400'000'000, 3, lab::core::Direction::Tx, {3}});
    recorder.stop();

    lab::core::ReplaySource replay(path);
    std::mutex eventsMutex;
    std::condition_variable eventsReady;
    std::vector<lab::core::DataChunk> events;
    replay.setCallbacks({
        [&](const lab::core::DataChunk& event) {
            {
                std::scoped_lock lock(eventsMutex);
                events.push_back(event);
            }
            eventsReady.notify_all();
        },
        {},
        {},
        {}});
    require(replay.open(), "replay source opens raw log");
    require(replay.status().paused && replay.status().recordCount == 3,
            "replay opens paused with complete index");
    replay.setSpeed(10.0);
    replay.resume();
    {
        std::unique_lock lock(eventsMutex);
        require(eventsReady.wait_for(lock, std::chrono::seconds(1), [&] { return events.size() == 3; }),
                "replay emits all records at accelerated speed");
        require(events[0].sequence == 1 && events[1].sequence == 2 && events[2].sequence == 3,
                "replay preserves file order");
        require(events[1].receiveTimestamp == 1'200'000'000,
                "replay preserves original timestamp");
    }
    require(replay.status().atEnd && replay.status().paused, "replay pauses at end");

    replay.seekFraction(0.0);
    replay.setSpeed(1.0);
    {
        std::scoped_lock lock(eventsMutex);
        events.clear();
    }
    replay.resume();
    {
        std::unique_lock lock(eventsMutex);
        require(eventsReady.wait_for(lock, std::chrono::milliseconds(300), [&] { return !events.empty(); }),
                "seek allows replay from beginning");
    }
    replay.pause();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    {
        std::scoped_lock lock(eventsMutex);
        require(events.size() == 1, "pause prevents the next timed record");
        require(events.front().sequence == 1, "seek returns to first record");
    }
    replay.resume();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    {
        std::scoped_lock lock(eventsMutex);
        require(events.size() == 1, "resume preserves the remaining inter-record delay");
    }
    {
        std::unique_lock lock(eventsMutex);
        require(eventsReady.wait_for(lock, std::chrono::milliseconds(300), [&] {
                    return events.size() >= 2;
                }),
                "resume continues playback after the preserved delay");
    }
    replay.close();
    std::filesystem::remove(path);
}

}  // namespace

void runProtocolTests();

int main() {
    try {
        testRingBufferWrapAround();
        testTimeSeriesAndStatistics();
        testDerivedFieldEngine();
        testCsvSplitChunksAndInvalidLine();
        testMockDataSource();
        testSourceManager();
        testProcessingPipeline();
        testProcessingPipelineSeparatesSources();
        testRawRecorder();
        testTruncatedRawLogRecovery();
        testSessionRecorder();
        testReplaySourceControls();
        runProtocolTests();
        std::cout << "All Lab Debugger core tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Test failed: " << exception.what() << '\n';
        return 1;
    }
}

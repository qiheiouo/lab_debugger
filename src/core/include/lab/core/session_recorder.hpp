#pragma once

#include "lab/core/data_sample.hpp"
#include "lab/core/derived_field_engine.hpp"
#include "lab/core/frame_stream_parser.hpp"
#include "lab/core/raw_log_recorder.hpp"
#include "lab/core/threshold_alert_engine.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace lab::core {

struct SessionParserConfiguration {
    std::vector<std::string> csvFields;
    std::string protocolName;
    std::string protocolJson;
};

struct SessionSourceMetadata {
    std::string sourceId;
    std::string type;
    std::string displayName;
    std::vector<std::pair<std::string, std::string>> configuration;
    std::optional<SessionParserConfiguration> parser;
};

struct SessionStartOptions {
    std::string softwareVersion;
    std::string sessionName;
    std::string machineName;
    std::string operatingSystem;
    std::vector<SessionSourceMetadata> sources;
    std::vector<std::string> csvFields;
    std::vector<DerivedFieldDefinition> derivedFields;
    std::vector<ThresholdAlertDefinition> alertRules;
    std::string protocolName;
    std::string protocolJson;
};

struct SessionEvent {
    Timestamp timestamp{};
    std::string sourceId;
    std::string severity;
    std::string category;
    std::string message;
    std::uint64_t sequence{};
};

struct SessionRecorderStatistics {
    std::uint64_t rawChunks{};
    std::uint64_t rawBytes{};
    std::uint64_t samples{};
    std::uint64_t frames{};
    std::uint64_t events{};
    std::size_t pendingItems{};
};

class SessionRecorder {
public:
    SessionRecorder() = default;
    ~SessionRecorder();

    SessionRecorder(const SessionRecorder&) = delete;
    SessionRecorder& operator=(const SessionRecorder&) = delete;

    bool start(const std::filesystem::path& directory, SessionStartOptions options);
    void stop();
    void enqueueRaw(DataChunk chunk);
    void enqueueSample(DataSample sample);
    void enqueueFrame(FrameEvent frame);
    void enqueueEvent(SessionEvent event);
    void updateProtocolSnapshot(std::string name, std::string json, Timestamp timestamp);

    [[nodiscard]] bool isRecording() const noexcept;
    [[nodiscard]] std::filesystem::path directory() const;
    [[nodiscard]] std::string error() const;
    [[nodiscard]] SessionRecorderStatistics statistics() const;

private:
    struct ProtocolSnapshot {
        std::string name;
        std::string json;
        Timestamp timestamp{};
    };
    using PendingItem = std::variant<DataSample, FrameEvent, SessionEvent, ProtocolSnapshot>;

    void run(std::stop_token stopToken);
    void writeItem(const PendingItem& item);
    bool writeMetadata(const std::string& status, Timestamp endTime);
    bool writeConfigurationFiles();

    mutable std::mutex mutex_;
    std::condition_variable_any ready_;
    std::deque<PendingItem> queue_;
    std::filesystem::path directory_;
    SessionStartOptions options_;
    std::ofstream values_;
    std::ofstream frames_;
    std::ofstream events_;
    RawLogRecorder rawRecorder_;
    std::jthread worker_;
    Timestamp startTime_{};
    std::string error_;
    bool recording_{};
    std::uint64_t rawChunks_{};
    std::uint64_t rawBytes_{};
    std::uint64_t samples_{};
    std::uint64_t framesCount_{};
    std::uint64_t eventsCount_{};
};

}  // namespace lab::core

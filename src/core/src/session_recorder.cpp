#include "lab/core/session_recorder.hpp"

#include "lab/core/logger.hpp"
#include "lab/core/timestamp.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace lab::core {
namespace {

std::string jsonEscape(const std::string& value) {
    std::ostringstream output;
    for (const auto character : value) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (static_cast<unsigned char>(character) < 0x20U) {
                output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<int>(static_cast<unsigned char>(character))
                       << std::dec << std::setfill(' ');
            } else {
                output << character;
            }
        }
    }
    return output.str();
}

std::string csvEscape(const std::string& value) {
    if (value.find_first_of(",\"\r\n") == std::string::npos) {
        return value;
    }
    std::string result{"\""};
    for (const auto character : value) {
        result.push_back(character);
        if (character == '"') {
            result.push_back('"');
        }
    }
    result.push_back('"');
    return result;
}

std::string safeFileStem(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        result.push_back(std::isalnum(byte) || character == '-' || character == '_'
                             ? character
                             : '_');
    }
    if (result.empty()) {
        return "protocol";
    }
    return result.substr(0, 80);
}

}  // namespace

SessionRecorder::~SessionRecorder() {
    stop();
}

bool SessionRecorder::start(
    const std::filesystem::path& directory,
    SessionStartOptions options) {
    stop();

    std::error_code fileError;
    const auto directoryExists = std::filesystem::exists(directory, fileError);
    if (fileError) {
        std::scoped_lock lock(mutex_);
        error_ = "cannot inspect session directory: " + fileError.message();
        return false;
    }
    if (directoryExists && !std::filesystem::is_empty(directory, fileError)) {
        std::scoped_lock lock(mutex_);
        error_ = "session directory is not empty";
        return false;
    }
    if (!std::filesystem::create_directories(directory / "raw", fileError) && fileError) {
        std::scoped_lock lock(mutex_);
        error_ = "cannot create session raw directory: " + fileError.message();
        return false;
    }
    std::filesystem::create_directories(directory / "protocol", fileError);
    if (fileError) {
        std::scoped_lock lock(mutex_);
        error_ = "cannot create session protocol directory: " + fileError.message();
        return false;
    }
    std::filesystem::create_directories(directory / "configuration", fileError);
    if (fileError) {
        std::scoped_lock lock(mutex_);
        error_ = "cannot create session configuration directory: " + fileError.message();
        return false;
    }

    {
        std::scoped_lock lock(mutex_);
        directory_ = directory;
        options_ = std::move(options);
        startTime_ = nowTimestampNs();
        error_.clear();
        rawChunks_ = 0;
        rawBytes_ = 0;
        samples_ = 0;
        framesCount_ = 0;
        eventsCount_ = 0;
        queue_.clear();
        values_.open(directory_ / "values.csv", std::ios::trunc);
        frames_.open(directory_ / "frames.jsonl", std::ios::trunc);
        events_.open(directory_ / "events.jsonl", std::ios::trunc);
        if (!values_ || !frames_ || !events_) {
            error_ = "cannot create session values, frames, or events file";
            values_.close();
            frames_.close();
            events_.close();
            return false;
        }
        values_ << "timestamp_ns,source_id,sequence,field,value,unit\n";
    }

    if (!writeConfigurationFiles() || !writeMetadata("recording", 0) ||
        !rawRecorder_.start(directory / "raw" / "stream.ldraw")) {
        std::scoped_lock lock(mutex_);
        if (error_.empty()) {
            error_ = "cannot initialize session files";
        }
        values_.close();
        frames_.close();
        events_.close();
        return false;
    }

    {
        std::scoped_lock lock(mutex_);
        recording_ = true;
    }
    worker_ = std::jthread([this](std::stop_token token) { run(token); });
    Logger::instance().log(
        LogLevel::Info, "SessionRecorder", "Session recording started: " + directory.string());
    return true;
}

void SessionRecorder::stop() {
    {
        std::scoped_lock lock(mutex_);
        recording_ = false;
    }
    rawRecorder_.stop();
    if (worker_.joinable()) {
        worker_.request_stop();
        ready_.notify_all();
        worker_.join();
    }

    bool hadSession = false;
    {
        std::scoped_lock lock(mutex_);
        hadSession = values_.is_open() || frames_.is_open() || events_.is_open();
        if (values_.is_open()) {
            values_.flush();
            values_.close();
        }
        if (events_.is_open()) {
            events_.flush();
            events_.close();
        }
        if (frames_.is_open()) {
            frames_.flush();
            frames_.close();
        }
        queue_.clear();
    }
    if (hadSession) {
        writeMetadata("completed", nowTimestampNs());
        Logger::instance().log(LogLevel::Info, "SessionRecorder", "Session recording stopped");
    }
}

void SessionRecorder::enqueueRaw(DataChunk chunk) {
    {
        std::scoped_lock lock(mutex_);
        if (!recording_) {
            return;
        }
        ++rawChunks_;
        rawBytes_ += chunk.payload.size();
    }
    rawRecorder_.enqueue(std::move(chunk));
}

void SessionRecorder::enqueueSample(DataSample sample) {
    {
        std::scoped_lock lock(mutex_);
        if (!recording_) {
            return;
        }
        queue_.emplace_back(std::move(sample));
    }
    ready_.notify_one();
}

void SessionRecorder::enqueueFrame(FrameEvent frame) {
    {
        std::scoped_lock lock(mutex_);
        if (!recording_) {
            return;
        }
        queue_.emplace_back(std::move(frame));
    }
    ready_.notify_one();
}

void SessionRecorder::enqueueEvent(SessionEvent event) {
    {
        std::scoped_lock lock(mutex_);
        if (!recording_) {
            return;
        }
        queue_.emplace_back(std::move(event));
    }
    ready_.notify_one();
}

void SessionRecorder::updateProtocolSnapshot(
    std::string name,
    std::string json,
    Timestamp timestamp) {
    {
        std::scoped_lock lock(mutex_);
        if (!recording_) {
            return;
        }
        queue_.emplace_back(ProtocolSnapshot{std::move(name), std::move(json), timestamp});
    }
    ready_.notify_one();
}

bool SessionRecorder::isRecording() const noexcept {
    std::scoped_lock lock(mutex_);
    return recording_;
}

std::filesystem::path SessionRecorder::directory() const {
    std::scoped_lock lock(mutex_);
    return directory_;
}

std::string SessionRecorder::error() const {
    std::scoped_lock lock(mutex_);
    return error_;
}

SessionRecorderStatistics SessionRecorder::statistics() const {
    std::scoped_lock lock(mutex_);
    return {rawChunks_, rawBytes_, samples_, framesCount_, eventsCount_, queue_.size()};
}

void SessionRecorder::run(std::stop_token stopToken) {
    while (true) {
        PendingItem item;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, stopToken, [this] { return !queue_.empty(); });
            if (queue_.empty()) {
                if (stopToken.stop_requested()) {
                    break;
                }
                continue;
            }
            item = std::move(queue_.front());
            queue_.pop_front();
        }
        writeItem(item);
    }
    while (true) {
        PendingItem item;
        {
            std::scoped_lock lock(mutex_);
            if (queue_.empty()) {
                break;
            }
            item = std::move(queue_.front());
            queue_.pop_front();
        }
        writeItem(item);
    }
}

void SessionRecorder::writeItem(const PendingItem& item) {
    if (const auto* sample = std::get_if<DataSample>(&item)) {
        values_ << sample->timestamp << ',' << csvEscape(sample->sourceId) << ','
                << sample->sequence << ',' << csvEscape(sample->field) << ','
                << std::setprecision(17) << sample->value << ','
                << csvEscape(sample->unit) << '\n';
        {
            std::scoped_lock lock(mutex_);
            ++samples_;
        }
        return;
    }
    if (const auto* frame = std::get_if<FrameEvent>(&item)) {
        frames_ << "{\"timestamp_ns\":" << frame->sourceTimestamp
                << ",\"source_id\":\"" << jsonEscape(frame->sourceId)
                << "\",\"sequence\":" << frame->sequence
                << ",\"fields\":[";
        for (std::size_t index = 0; index < frame->fields.size(); ++index) {
            const auto& field = frame->fields[index];
            frames_ << (index == 0 ? "" : ",")
                    << "{\"name\":\"" << jsonEscape(field.name)
                    << "\",\"type\":\"" << jsonEscape(toString(field.type))
                    << "\",\"value\":\"" << jsonEscape(decodedValueToString(field))
                    << "\",\"unit\":\"" << jsonEscape(field.unit)
                    << "\",\"enum\":\"" << jsonEscape(field.enumLabel) << "\"}";
        }
        frames_ << "]}\n";
        {
            std::scoped_lock lock(mutex_);
            ++framesCount_;
        }
        return;
    }
    if (const auto* event = std::get_if<SessionEvent>(&item)) {
        events_ << "{\"timestamp_ns\":" << event->timestamp
                << ",\"source_id\":\"" << jsonEscape(event->sourceId)
                << "\",\"sequence\":" << event->sequence
                << ",\"severity\":\"" << jsonEscape(event->severity)
                << "\",\"category\":\"" << jsonEscape(event->category)
                << "\",\"message\":\"" << jsonEscape(event->message) << "\"}\n";
        {
            std::scoped_lock lock(mutex_);
            ++eventsCount_;
        }
        return;
    }

    const auto& snapshot = std::get<ProtocolSnapshot>(item);
    const auto name = std::to_string(snapshot.timestamp) + "_" +
                      safeFileStem(snapshot.name) + ".json";
    std::filesystem::path root;
    {
        std::scoped_lock lock(mutex_);
        root = directory_;
    }
    std::ofstream protocolFile(root / "protocol" / name, std::ios::trunc);
    protocolFile << snapshot.json;
}

bool SessionRecorder::writeMetadata(const std::string& status, Timestamp endTime) {
    std::filesystem::path path;
    SessionStartOptions options;
    Timestamp startTime{};
    SessionRecorderStatistics stats;
    {
        std::scoped_lock lock(mutex_);
        path = directory_ / "metadata.json";
        options = options_;
        startTime = startTime_;
        stats = {rawChunks_, rawBytes_, samples_, framesCount_, eventsCount_, queue_.size()};
    }

    std::ofstream metadata(path, std::ios::trunc);
    if (!metadata) {
        std::scoped_lock lock(mutex_);
        error_ = "cannot write session metadata";
        return false;
    }
    metadata << "{\n"
             << "  \"format\": \"lab-debug-session\",\n"
             << "  \"format_version\": 1,\n"
             << "  \"status\": \"" << status << "\",\n"
             << "  \"name\": \"" << jsonEscape(options.sessionName) << "\",\n"
             << "  \"software_version\": \"" << jsonEscape(options.softwareVersion) << "\",\n"
             << "  \"start_time_ns\": " << startTime << ",\n"
             << "  \"end_time_ns\": " << endTime << ",\n"
             << "  \"machine\": {\"name\": \"" << jsonEscape(options.machineName)
             << "\", \"os\": \"" << jsonEscape(options.operatingSystem) << "\"},\n"
             << "  \"protocol\": {\"name\": \"" << jsonEscape(options.protocolName)
             << "\", \"snapshot\": "
             << (options.protocolJson.empty() ? "null" : "\"protocol/initial.json\"") << "},\n"
             << "  \"derived_fields\": \"configuration/derived_fields.json\",\n"
             << "  \"alert_rules\": \"configuration/alert_rules.json\",\n"
             << "  \"sources\": [";
    for (std::size_t index = 0; index < options.sources.size(); ++index) {
        const auto& source = options.sources[index];
        metadata << (index == 0 ? "\n" : ",\n")
                 << "    {\"id\": \"" << jsonEscape(source.sourceId)
                 << "\", \"type\": \"" << jsonEscape(source.type)
                 << "\", \"name\": \"" << jsonEscape(source.displayName) << "\"}";
    }
    if (!options.sources.empty()) {
        metadata << '\n';
    }
    metadata << "  ],\n"
             << "  \"counts\": {\"raw_chunks\": " << stats.rawChunks
             << ", \"raw_bytes\": " << stats.rawBytes
             << ", \"samples\": " << stats.samples
             << ", \"frames\": " << stats.frames
             << ", \"events\": " << stats.events << "}\n"
             << "}\n";
    return static_cast<bool>(metadata);
}

bool SessionRecorder::writeConfigurationFiles() {
    std::filesystem::path root;
    SessionStartOptions options;
    {
        std::scoped_lock lock(mutex_);
        root = directory_;
        options = options_;
    }

    if (!options.protocolJson.empty()) {
        std::ofstream protocol(root / "protocol" / "initial.json", std::ios::trunc);
        if (!protocol) {
            std::scoped_lock lock(mutex_);
            error_ = "cannot write protocol snapshot";
            return false;
        }
        protocol << options.protocolJson;
    }

    {
        std::ofstream fields(root / "configuration" / "csv_fields.txt", std::ios::trunc);
        if (!fields) {
            std::scoped_lock lock(mutex_);
            error_ = "cannot write CSV field configuration";
            return false;
        }
        for (const auto& field : options.csvFields) {
            fields << field << '\n';
        }
    }

    {
        std::ofstream derived(
            root / "configuration" / "derived_fields.json", std::ios::trunc);
        if (!derived) {
            std::scoped_lock lock(mutex_);
            error_ = "cannot write derived field configuration";
            return false;
        }
        derived << "{\n  \"format_version\": 1,\n  \"fields\": [";
        for (std::size_t index = 0; index < options.derivedFields.size(); ++index) {
            const auto& field = options.derivedFields[index];
            derived << (index == 0 ? "\n" : ",\n")
                    << "    {\"name\": \"" << jsonEscape(field.name)
                    << "\", \"expression\": \"" << jsonEscape(field.expression)
                    << "\", \"unit\": \"" << jsonEscape(field.unit) << "\"}";
        }
        if (!options.derivedFields.empty()) derived << '\n';
        derived << "  ]\n}\n";
        if (!derived) {
            std::scoped_lock lock(mutex_);
            error_ = "cannot finish derived field configuration";
            return false;
        }
    }

    {
        std::ofstream alerts(
            root / "configuration" / "alert_rules.json", std::ios::trunc);
        if (!alerts) {
            std::scoped_lock lock(mutex_);
            error_ = "cannot write threshold alert configuration";
            return false;
        }
        alerts << "{\n  \"format_version\": 1,\n  \"rules\": [";
        for (std::size_t index = 0; index < options.alertRules.size(); ++index) {
            const auto& rule = options.alertRules[index];
            alerts << (index == 0 ? "\n" : ",\n")
                   << "    {\"name\": \"" << jsonEscape(rule.name)
                   << "\", \"field\": \"" << jsonEscape(rule.field)
                   << "\", \"comparison\": \"" << toString(rule.comparison)
                   << "\", \"threshold\": " << std::setprecision(17)
                   << rule.threshold << ", \"hysteresis\": " << rule.hysteresis
                   << ", \"message\": \"" << jsonEscape(rule.message) << "\"}";
        }
        if (!options.alertRules.empty()) alerts << '\n';
        alerts << "  ]\n}\n";
        if (!alerts) {
            std::scoped_lock lock(mutex_);
            error_ = "cannot finish threshold alert configuration";
            return false;
        }
    }

    for (std::size_t index = 0; index < options.sources.size(); ++index) {
        const auto& source = options.sources[index];
        std::ofstream configuration(
            root / "configuration" / ("source_" + std::to_string(index) + ".json"),
            std::ios::trunc);
        if (!configuration) {
            std::scoped_lock lock(mutex_);
            error_ = "cannot write source configuration";
            return false;
        }
        configuration << "{\n  \"id\": \"" << jsonEscape(source.sourceId)
                      << "\",\n  \"type\": \"" << jsonEscape(source.type)
                      << "\",\n  \"name\": \"" << jsonEscape(source.displayName)
                      << "\",\n  \"configuration\": {";
        for (std::size_t field = 0; field < source.configuration.size(); ++field) {
            configuration << (field == 0 ? "\n" : ",\n")
                          << "    \"" << jsonEscape(source.configuration[field].first)
                          << "\": \"" << jsonEscape(source.configuration[field].second) << "\"";
        }
        if (!source.configuration.empty()) {
            configuration << '\n';
        }
        configuration << "  }\n}\n";
    }
    return true;
}

}  // namespace lab::core

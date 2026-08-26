#include "lab/core/raw_log_recorder.hpp"

#include "lab/core/logger.hpp"

#include <array>
#include <cstdint>
#include <limits>

namespace lab::core {
namespace {

template <typename T>
void writeValue(std::ofstream& stream, T value) {
    stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

}  // namespace

RawLogRecorder::~RawLogRecorder() {
    stop();
}

bool RawLogRecorder::start(const std::filesystem::path& path) {
    stop();
    {
        std::scoped_lock lock(mutex_);
        output_.open(path, std::ios::binary | std::ios::trunc);
        if (!output_) {
            Logger::instance().log(
                LogLevel::Error, "Recorder", "Cannot open raw log: " + path.string());
            return false;
        }
        constexpr std::array<char, 8> magic{'L', 'D', 'B', 'G', 'R', 'A', 'W', '1'};
        output_.write(magic.data(), magic.size());
        recording_ = true;
    }
    worker_ = std::jthread([this](std::stop_token token) { run(token); });
    Logger::instance().log(
        LogLevel::Info, "Recorder", "Raw recording started: " + path.string());
    return true;
}

void RawLogRecorder::stop() {
    {
        std::scoped_lock lock(mutex_);
        // Reject new chunks before draining the queue, so every chunk accepted
        // before stop() has a well-defined chance to reach disk.
        recording_ = false;
    }
    if (worker_.joinable()) {
        worker_.request_stop();
        ready_.notify_all();
        worker_.join();
    }
    std::scoped_lock lock(mutex_);
    queue_.clear();
    if (output_.is_open()) {
        output_.flush();
        output_.close();
        Logger::instance().log(LogLevel::Info, "Recorder", "Raw recording stopped");
    }
}

void RawLogRecorder::enqueue(DataChunk chunk) {
    {
        std::scoped_lock lock(mutex_);
        if (!recording_) {
            return;
        }
        queue_.push_back(std::move(chunk));
    }
    ready_.notify_one();
}

bool RawLogRecorder::isRecording() const noexcept {
    std::scoped_lock lock(mutex_);
    return recording_;
}

std::size_t RawLogRecorder::pendingChunks() const {
    std::scoped_lock lock(mutex_);
    return queue_.size();
}

void RawLogRecorder::run(std::stop_token stopToken) {
    while (true) {
        DataChunk chunk;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, stopToken, [this] { return !queue_.empty(); });
            if (queue_.empty()) {
                if (stopToken.stop_requested()) {
                    break;
                }
                continue;
            }
            chunk = std::move(queue_.front());
            queue_.pop_front();
        }
        writeChunk(chunk);
    }

    // A requested stop drains data already accepted by enqueue().
    while (true) {
        DataChunk chunk;
        {
            std::scoped_lock lock(mutex_);
            if (queue_.empty()) {
                break;
            }
            chunk = std::move(queue_.front());
            queue_.pop_front();
        }
        writeChunk(chunk);
    }
}

void RawLogRecorder::writeChunk(const DataChunk& chunk) {
    std::scoped_lock lock(mutex_);
    if (!output_) {
        return;
    }
    if (chunk.sourceId.size() > std::numeric_limits<std::uint32_t>::max() ||
        chunk.payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        Logger::instance().log(LogLevel::Error, "Recorder", "Raw record is too large");
        return;
    }
    constexpr std::uint32_t recordMagic = 0x4C444252;
    writeValue(output_, recordMagic);
    writeValue(output_, chunk.sourceTimestamp);
    writeValue(output_, chunk.receiveTimestamp);
    writeValue(output_, chunk.sequence);
    writeValue(output_, static_cast<std::uint8_t>(chunk.direction));
    writeValue(output_, static_cast<std::uint32_t>(chunk.sourceId.size()));
    writeValue(output_, static_cast<std::uint32_t>(chunk.payload.size()));
    output_.write(chunk.sourceId.data(), static_cast<std::streamsize>(chunk.sourceId.size()));
    output_.write(
        reinterpret_cast<const char*>(chunk.payload.data()),
        static_cast<std::streamsize>(chunk.payload.size()));
}

}  // namespace lab::core

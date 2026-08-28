#include "lab/core/raw_log_recorder.hpp"

#include "lab/core/logger.hpp"

namespace lab::core {

RawLogRecorder::~RawLogRecorder() {
    stop();
}

bool RawLogRecorder::start(const std::filesystem::path& path) {
    stop();
    {
        std::scoped_lock lock(mutex_);
        if (!writer_.open(path)) {
            Logger::instance().log(
                LogLevel::Error,
                "Recorder",
                "Cannot open raw log: " + path.string() + ": " + writer_.error());
            return false;
        }
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
    if (writer_.isOpen()) {
        writer_.close();
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
    if (!writer_.write(chunk)) {
        Logger::instance().log(LogLevel::Error, "Recorder", writer_.error());
    }
}

}  // namespace lab::core

#pragma once

#include "lab/core/data_chunk.hpp"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stop_token>
#include <thread>

namespace lab::core {

class RawLogRecorder {
public:
    RawLogRecorder() = default;
    ~RawLogRecorder();

    RawLogRecorder(const RawLogRecorder&) = delete;
    RawLogRecorder& operator=(const RawLogRecorder&) = delete;

    bool start(const std::filesystem::path& path);
    void stop();
    void enqueue(DataChunk chunk);

    [[nodiscard]] bool isRecording() const noexcept;
    [[nodiscard]] std::size_t pendingChunks() const;

private:
    void run(std::stop_token stopToken);
    void writeChunk(const DataChunk& chunk);

    mutable std::mutex mutex_;
    std::condition_variable_any ready_;
    std::deque<DataChunk> queue_;
    std::ofstream output_;
    std::jthread worker_;
    bool recording_{};
};

}  // namespace lab::core


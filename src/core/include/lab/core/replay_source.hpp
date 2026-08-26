#pragma once

#include "lab/core/data_source.hpp"
#include "lab/core/raw_log_reader.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <stop_token>
#include <thread>

namespace lab::core {

struct ReplayStatus {
    bool open{};
    bool paused{true};
    bool atEnd{};
    double speed{1.0};
    std::size_t position{};
    std::size_t recordCount{};
    Timestamp firstTimestamp{};
    Timestamp lastTimestamp{};
    Timestamp currentTimestamp{};
    bool recoveredTruncatedTail{};
};

class ReplaySource final : public IDataSource {
public:
    explicit ReplaySource(std::filesystem::path rawLogPath = {});
    ~ReplaySource() override;

    ReplaySource(const ReplaySource&) = delete;
    ReplaySource& operator=(const ReplaySource&) = delete;

    void setPath(std::filesystem::path rawLogPath);
    [[nodiscard]] std::filesystem::path path() const;

    bool open() override;
    void close() override;
    [[nodiscard]] bool isOpen() const noexcept override;
    bool write(std::span<const std::uint8_t> data) override;
    [[nodiscard]] std::string sourceId() const override;
    [[nodiscard]] SourceStatistics statistics() const noexcept override;

    void pause();
    void resume();
    void setSpeed(double speed);
    void seek(Timestamp timelineTimestamp);
    void seekFraction(double fraction);
    [[nodiscard]] ReplayStatus status() const;

private:
    void run(std::stop_token stopToken);

    mutable std::mutex mutex_;
    std::condition_variable_any controlChanged_;
    std::filesystem::path path_;
    RawLogReader reader_;
    std::jthread worker_;
    bool open_{};
    bool paused_{true};
    bool atEnd_{};
    double speed_{1.0};
    std::size_t position_{};
    Timestamp currentTimestamp_{};
    std::uint64_t controlVersion_{};
    SourceStatistics statistics_;
};

}  // namespace lab::core

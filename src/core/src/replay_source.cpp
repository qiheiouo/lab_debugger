#include "lab/core/replay_source.hpp"

#include <algorithm>
#include <chrono>

namespace lab::core {

ReplaySource::ReplaySource(std::filesystem::path rawLogPath)
    : path_(std::move(rawLogPath)) {}

ReplaySource::~ReplaySource() {
    close();
}

void ReplaySource::setPath(std::filesystem::path rawLogPath) {
    close();
    std::scoped_lock lock(mutex_);
    path_ = std::move(rawLogPath);
}

std::filesystem::path ReplaySource::path() const {
    std::scoped_lock lock(mutex_);
    return path_;
}

bool ReplaySource::open() {
    close();
    publishState(SourceState::Opening);

    std::filesystem::path rawPath;
    {
        std::scoped_lock lock(mutex_);
        rawPath = path_;
    }
    if (rawPath.empty() || !reader_.open(rawPath)) {
        const auto message = rawPath.empty() ? "replay path is empty" : reader_.error();
        {
            std::scoped_lock lock(mutex_);
            ++statistics_.errors;
        }
        publishError("Replay open failed: " + message);
        publishState(SourceState::Error);
        return false;
    }

    {
        std::scoped_lock lock(mutex_);
        statistics_ = {};
        position_ = 0;
        currentTimestamp_ = reader_.firstTimestamp();
        paused_ = true;
        atEnd_ = reader_.recordCount() == 0;
        open_ = true;
        ++controlVersion_;
    }
    worker_ = std::jthread([this](std::stop_token token) { run(token); });
    publishState(SourceState::Open);
    if (reader_.hasTruncatedTail()) {
        publishError("Replay recovered all complete records before a truncated tail");
    }
    return true;
}

void ReplaySource::close() {
    bool wasOpen = false;
    {
        std::scoped_lock lock(mutex_);
        wasOpen = open_;
        open_ = false;
        paused_ = true;
        ++controlVersion_;
    }
    if (wasOpen) {
        publishState(SourceState::Closing);
    }
    if (worker_.joinable()) {
        worker_.request_stop();
        controlChanged_.notify_all();
        worker_.join();
    }
    reader_.close();
    if (wasOpen) {
        publishState(SourceState::Closed);
    }
}

bool ReplaySource::isOpen() const noexcept {
    std::scoped_lock lock(mutex_);
    return open_;
}

bool ReplaySource::write(std::span<const std::uint8_t>) {
    publishError("ReplaySource is read-only");
    return false;
}

std::string ReplaySource::sourceId() const {
    return "replay";
}

SourceStatistics ReplaySource::statistics() const noexcept {
    std::scoped_lock lock(mutex_);
    return statistics_;
}

void ReplaySource::pause() {
    {
        std::scoped_lock lock(mutex_);
        if (!open_) {
            return;
        }
        paused_ = true;
        ++controlVersion_;
    }
    controlChanged_.notify_all();
}

void ReplaySource::resume() {
    {
        std::scoped_lock lock(mutex_);
        if (!open_ || atEnd_) {
            return;
        }
        paused_ = false;
        ++controlVersion_;
    }
    controlChanged_.notify_all();
}

void ReplaySource::setSpeed(double speed) {
    {
        std::scoped_lock lock(mutex_);
        speed_ = std::clamp(speed, 0.1, 10.0);
        ++controlVersion_;
    }
    controlChanged_.notify_all();
}

void ReplaySource::seek(Timestamp timelineTimestamp) {
    const auto newPosition = reader_.lowerBound(timelineTimestamp);
    {
        std::scoped_lock lock(mutex_);
        if (!open_) {
            return;
        }
        position_ = newPosition;
        atEnd_ = position_ >= reader_.recordCount();
        if (reader_.recordCount() > 0) {
            const auto index = std::min(position_, reader_.recordCount() - 1);
            currentTimestamp_ = reader_.index()[index].timelineTimestamp;
        }
        ++controlVersion_;
    }
    controlChanged_.notify_all();
}

void ReplaySource::seekFraction(double fraction) {
    const auto first = reader_.firstTimestamp();
    const auto duration = reader_.duration();
    const auto clamped = std::clamp(fraction, 0.0, 1.0);
    seek(first + static_cast<Timestamp>(static_cast<double>(duration) * clamped));
}

ReplayStatus ReplaySource::status() const {
    std::scoped_lock lock(mutex_);
    return {open_,
            paused_,
            atEnd_,
            speed_,
            position_,
            reader_.recordCount(),
            reader_.firstTimestamp(),
            reader_.lastTimestamp(),
            currentTimestamp_,
            reader_.hasTruncatedTail()};
}

void ReplaySource::run(std::stop_token stopToken) {
    while (!stopToken.stop_requested()) {
        std::size_t recordIndex{};
        std::uint64_t version{};
        double speed{};
        Timestamp baseTimestamp{};
        std::chrono::steady_clock::time_point baseTime;
        {
            std::unique_lock lock(mutex_);
            controlChanged_.wait(lock, stopToken, [this] {
                return !open_ || (!paused_ && !atEnd_);
            });
            if (!open_ || stopToken.stop_requested()) {
                break;
            }
            recordIndex = position_;
            version = controlVersion_;
            speed = speed_;
            baseTimestamp = currentTimestamp_;
            baseTime = std::chrono::steady_clock::now();
        }

        while (!stopToken.stop_requested()) {
            RawLogIndexEntry entry;
            {
                std::unique_lock lock(mutex_);
                if (!open_ || paused_ || atEnd_ || controlVersion_ != version) {
                    break;
                }
                if (position_ >= reader_.recordCount()) {
                    atEnd_ = true;
                    paused_ = true;
                    break;
                }
                recordIndex = position_;
                entry = reader_.index()[recordIndex];
                const auto offset = entry.timelineTimestamp - baseTimestamp;
                const auto waitNanoseconds = static_cast<std::int64_t>(
                    static_cast<double>(offset) / speed);
                const auto target = baseTime + std::chrono::nanoseconds(waitNanoseconds);
                const auto interrupted = controlChanged_.wait_until(
                    lock, stopToken, target,
                    [this, version] {
                        return !open_ || paused_ || controlVersion_ != version;
                    });
                if (interrupted || !open_ || paused_ || controlVersion_ != version) {
                    break;
                }
            }

            auto chunk = reader_.read(recordIndex);
            if (!chunk) {
                {
                    std::scoped_lock lock(mutex_);
                    ++statistics_.errors;
                    paused_ = true;
                    atEnd_ = true;
                }
                publishError("Replay record read failed: " + reader_.error());
                break;
            }
            publishData(*chunk);

            {
                std::scoped_lock lock(mutex_);
                if (controlVersion_ != version || position_ != recordIndex) {
                    continue;
                }
                ++position_;
                currentTimestamp_ = entry.timelineTimestamp;
                if (chunk->direction == Direction::Rx) {
                    statistics_.receivedBytes += chunk->payload.size();
                    ++statistics_.receivedChunks;
                } else {
                    statistics_.transmittedBytes += chunk->payload.size();
                    ++statistics_.transmittedChunks;
                }
                if (position_ >= reader_.recordCount()) {
                    atEnd_ = true;
                    paused_ = true;
                    ++controlVersion_;
                }
            }
        }
    }
}

}  // namespace lab::core

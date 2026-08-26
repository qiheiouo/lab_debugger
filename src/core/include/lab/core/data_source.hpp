#pragma once

#include "lab/core/data_chunk.hpp"
#include "lab/core/data_sample.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>

namespace lab::core {

enum class SourceState { Closed, Opening, Open, Closing, Error };

struct SourceStatistics {
    std::uint64_t receivedBytes{};
    std::uint64_t transmittedBytes{};
    std::uint64_t receivedChunks{};
    std::uint64_t transmittedChunks{};
    std::uint64_t errors{};
};

struct DataSourceCallbacks {
    std::function<void(const DataChunk&)> onData;
    std::function<void(SourceState)> onStateChanged;
    std::function<void(const std::string&)> onError;
    std::function<void(const DataSample&)> onSample;
};

class IDataSource {
public:
    virtual ~IDataSource() = default;

    virtual bool open() = 0;
    virtual void close() = 0;
    [[nodiscard]] virtual bool isOpen() const noexcept = 0;
    virtual bool write(std::span<const std::uint8_t> data) = 0;
    [[nodiscard]] virtual std::string sourceId() const = 0;
    [[nodiscard]] virtual SourceStatistics statistics() const noexcept = 0;

    void setCallbacks(DataSourceCallbacks callbacks) {
        std::scoped_lock lock(callbackMutex_);
        callbacks_ = std::move(callbacks);
    }

protected:
    void publishData(const DataChunk& chunk) const {
        auto callback = dataCallback();
        if (callback) {
            callback(chunk);
        }
    }

    void publishSample(const DataSample& sample) const {
        std::function<void(const DataSample&)> callback;
        {
            std::scoped_lock lock(callbackMutex_);
            callback = callbacks_.onSample;
        }
        if (callback) {
            callback(sample);
        }
    }

    void publishState(SourceState state) const {
        std::function<void(SourceState)> callback;
        {
            std::scoped_lock lock(callbackMutex_);
            callback = callbacks_.onStateChanged;
        }
        if (callback) {
            callback(state);
        }
    }

    void publishError(const std::string& message) const {
        std::function<void(const std::string&)> callback;
        {
            std::scoped_lock lock(callbackMutex_);
            callback = callbacks_.onError;
        }
        if (callback) {
            callback(message);
        }
    }

private:
    [[nodiscard]] std::function<void(const DataChunk&)> dataCallback() const {
        std::scoped_lock lock(callbackMutex_);
        return callbacks_.onData;
    }

    mutable std::mutex callbackMutex_;
    DataSourceCallbacks callbacks_;
};

}  // namespace lab::core

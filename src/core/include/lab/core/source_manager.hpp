#pragma once

#include "lab/core/data_source.hpp"

#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace lab::core {

struct ManagedSourceInfo {
    std::string key;
    std::string sourceId;
    bool open{};
    SourceStatistics statistics;
};

struct SourceManagerCallbacks {
    std::function<void(const std::string&, const DataChunk&)> onData;
    std::function<void(const std::string&, const std::string&, SourceState)> onStateChanged;
    std::function<void(const std::string&, const std::string&, const std::string&)> onError;
    std::function<void(const std::string&, const DataSample&)> onSample;
};

class SourceManager {
public:
    SourceManager() = default;
    ~SourceManager();

    SourceManager(const SourceManager&) = delete;
    SourceManager& operator=(const SourceManager&) = delete;

    bool add(std::string key, IDataSource& source);
    void setCallbacks(SourceManagerCallbacks callbacks);

    bool open(const std::string& key);
    void close(const std::string& key);
    void closeAll();
    [[nodiscard]] bool isOpen(const std::string& key) const;
    bool write(const std::string& key, std::span<const std::uint8_t> data);

    [[nodiscard]] std::vector<ManagedSourceInfo> sources() const;
    [[nodiscard]] SourceStatistics aggregateStatistics() const;

private:
    [[nodiscard]] IDataSource* find(const std::string& key) const;
    void dispatchData(const std::string& key, const DataChunk& chunk) const;
    void dispatchState(const std::string& key, SourceState state) const;
    void dispatchError(const std::string& key, const std::string& message) const;
    void dispatchSample(const std::string& key, const DataSample& sample) const;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, IDataSource*> sources_;
    SourceManagerCallbacks callbacks_;
};

}  // namespace lab::core

#include "lab/core/source_manager.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace lab::core {
namespace {

void saturatingAdd(std::uint64_t& destination, std::uint64_t value) noexcept {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    destination = value > maximum - destination ? maximum : destination + value;
}

}  // namespace

SourceManager::~SourceManager() {
    closeAll();
    std::vector<IDataSource*> sources;
    {
        std::scoped_lock lock(mutex_);
        sources.reserve(sources_.size());
        for (const auto& [key, source] : sources_) {
            static_cast<void>(key);
            sources.push_back(source);
        }
        sources_.clear();
        callbacks_ = {};
    }
    for (auto* source : sources) {
        source->setCallbacks({});
    }
}

bool SourceManager::add(std::string key, IDataSource& source) {
    if (key.empty()) {
        return false;
    }
    std::scoped_lock lock(mutex_);
    const auto sourceAlreadyManaged = std::ranges::any_of(
        sources_, [&source](const auto& item) { return item.second == &source; });
    if (sources_.contains(key) || sourceAlreadyManaged) {
        return false;
    }
    sources_.emplace(key, &source);
    source.setCallbacks({
        [this, key](const DataChunk& chunk) { dispatchData(key, chunk); },
        [this, key](SourceState state) { dispatchState(key, state); },
        [this, key](const std::string& message) { dispatchError(key, message); },
        [this, key](const DataSample& sample) { dispatchSample(key, sample); },
        [this, key](std::span<const DataSample> samples) {
            dispatchSamples(key, samples);
        }});
    return true;
}

bool SourceManager::remove(const std::string& key) {
    IDataSource* source = nullptr;
    {
        std::scoped_lock lock(mutex_);
        const auto found = sources_.find(key);
        if (found == sources_.end()) {
            return false;
        }
        source = found->second;
    }

    source->close();
    source->setCallbacks({});
    std::scoped_lock lock(mutex_);
    const auto found = sources_.find(key);
    if (found == sources_.end() || found->second != source) {
        return false;
    }
    sources_.erase(found);
    return true;
}

void SourceManager::setCallbacks(SourceManagerCallbacks callbacks) {
    std::scoped_lock lock(mutex_);
    callbacks_ = std::move(callbacks);
}

bool SourceManager::open(const std::string& key) {
    auto* source = find(key);
    return source != nullptr && source->open();
}

void SourceManager::close(const std::string& key) {
    if (auto* source = find(key)) {
        source->close();
    }
}

void SourceManager::closeAll() {
    std::vector<IDataSource*> sources;
    {
        std::scoped_lock lock(mutex_);
        sources.reserve(sources_.size());
        for (const auto& [key, source] : sources_) {
            static_cast<void>(key);
            sources.push_back(source);
        }
    }
    for (auto* source : sources) {
        source->close();
    }
}

bool SourceManager::isOpen(const std::string& key) const {
    const auto* source = find(key);
    return source != nullptr && source->isOpen();
}

bool SourceManager::write(const std::string& key, std::span<const std::uint8_t> data) {
    auto* source = find(key);
    return source != nullptr && source->write(data);
}

std::vector<ManagedSourceInfo> SourceManager::sources() const {
    std::vector<std::pair<std::string, IDataSource*>> sources;
    {
        std::scoped_lock lock(mutex_);
        sources.reserve(sources_.size());
        for (const auto& item : sources_) {
            sources.push_back(item);
        }
    }
    std::ranges::sort(sources, {}, &std::pair<std::string, IDataSource*>::first);

    std::vector<ManagedSourceInfo> result;
    result.reserve(sources.size());
    for (const auto& [key, source] : sources) {
        result.push_back({key, source->sourceId(), source->isOpen(), source->statistics()});
    }
    return result;
}

SourceStatistics SourceManager::aggregateStatistics() const {
    SourceStatistics result;
    for (const auto& source : sources()) {
        saturatingAdd(result.receivedBytes, source.statistics.receivedBytes);
        saturatingAdd(result.transmittedBytes, source.statistics.transmittedBytes);
        saturatingAdd(result.receivedChunks, source.statistics.receivedChunks);
        saturatingAdd(result.transmittedChunks, source.statistics.transmittedChunks);
        saturatingAdd(result.errors, source.statistics.errors);
    }
    return result;
}

IDataSource* SourceManager::find(const std::string& key) const {
    std::scoped_lock lock(mutex_);
    const auto source = sources_.find(key);
    return source == sources_.end() ? nullptr : source->second;
}

void SourceManager::dispatchData(const std::string& key, const DataChunk& chunk) const {
    std::function<void(const std::string&, const DataChunk&)> callback;
    {
        std::scoped_lock lock(mutex_);
        callback = callbacks_.onData;
    }
    if (callback) {
        callback(key, chunk);
    }
}

void SourceManager::dispatchState(const std::string& key, SourceState state) const {
    std::function<void(const std::string&, const std::string&, SourceState)> callback;
    IDataSource* source = nullptr;
    {
        std::scoped_lock lock(mutex_);
        callback = callbacks_.onStateChanged;
        const auto item = sources_.find(key);
        if (item != sources_.end()) {
            source = item->second;
        }
    }
    if (callback) {
        callback(key, source != nullptr ? source->sourceId() : std::string{}, state);
    }
}

void SourceManager::dispatchError(const std::string& key, const std::string& message) const {
    std::function<void(const std::string&, const std::string&, const std::string&)> callback;
    IDataSource* source = nullptr;
    {
        std::scoped_lock lock(mutex_);
        callback = callbacks_.onError;
        const auto item = sources_.find(key);
        if (item != sources_.end()) {
            source = item->second;
        }
    }
    if (callback) {
        callback(key, source != nullptr ? source->sourceId() : std::string{}, message);
    }
}

void SourceManager::dispatchSample(const std::string& key, const DataSample& sample) const {
    std::function<void(const std::string&, const DataSample&)> callback;
    {
        std::scoped_lock lock(mutex_);
        callback = callbacks_.onSample;
    }
    if (callback) {
        callback(key, sample);
    }
}

void SourceManager::dispatchSamples(const std::string& key,
                                    std::span<const DataSample> samples) const {
    std::function<void(const std::string&, std::span<const DataSample>)> callback;
    {
        std::scoped_lock lock(mutex_);
        callback = callbacks_.onSamples;
    }
    if (callback) callback(key, samples);
}

}  // namespace lab::core

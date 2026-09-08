#include "lab/core/mock_data_source.hpp"

namespace lab::core {

MockDataSource::MockDataSource(std::string id) : id_(std::move(id)) {}

bool MockDataSource::open() {
    publishState(SourceState::Opening);
    open_.store(true);
    publishState(SourceState::Open);
    return true;
}

void MockDataSource::close() {
    if (!open_.exchange(false)) {
        return;
    }
    publishState(SourceState::Closing);
    publishState(SourceState::Closed);
}

bool MockDataSource::isOpen() const noexcept {
    return open_.load();
}

bool MockDataSource::write(std::span<const std::uint8_t> data) {
    if (!isOpen()) {
        return false;
    }
    std::vector<std::uint8_t> copy(data.begin(), data.end());
    {
        std::scoped_lock lock(writesMutex_);
        writes_.push_back(copy);
    }
    transmittedBytes_.fetch_add(copy.size());
    transmittedChunks_.fetch_add(1);
    const auto now = nowTimestampNs();
    publishData(DataChunk{
        id_, now, now, sequence_.fetch_add(1), Direction::Tx, std::move(copy)});
    return true;
}

std::string MockDataSource::sourceId() const {
    return id_;
}

SourceStatistics MockDataSource::statistics() const noexcept {
    return SourceStatistics{
        receivedBytes_.load(),
        transmittedBytes_.load(),
        receivedChunks_.load(),
        transmittedChunks_.load(),
        0};
}

void MockDataSource::feed(
    std::span<const std::uint8_t> data,
    Timestamp sourceTimestamp) {
    if (!isOpen()) {
        return;
    }
    std::vector<std::uint8_t> copy(data.begin(), data.end());
    receivedBytes_.fetch_add(copy.size());
    receivedChunks_.fetch_add(1);
    const auto receiveTime = nowTimestampNs();
    publishData(DataChunk{
        id_,
        sourceTimestamp == 0 ? receiveTime : sourceTimestamp,
        receiveTime,
        sequence_.fetch_add(1),
        Direction::Rx,
        std::move(copy)});
}

void MockDataSource::feedSamples(std::span<const DataSample> samples) {
    if (isOpen()) publishSamples(samples);
}

std::vector<std::vector<std::uint8_t>> MockDataSource::writes() const {
    std::scoped_lock lock(writesMutex_);
    return writes_;
}

}  // namespace lab::core

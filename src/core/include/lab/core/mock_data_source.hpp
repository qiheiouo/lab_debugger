#pragma once

#include "lab/core/data_source.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace lab::core {

class MockDataSource final : public IDataSource {
public:
    explicit MockDataSource(std::string id = "mock:0");

    bool open() override;
    void close() override;
    [[nodiscard]] bool isOpen() const noexcept override;
    bool write(std::span<const std::uint8_t> data) override;
    [[nodiscard]] std::string sourceId() const override;
    [[nodiscard]] SourceStatistics statistics() const noexcept override;

    void feed(std::span<const std::uint8_t> data, Timestamp sourceTimestamp = 0);
    [[nodiscard]] std::vector<std::vector<std::uint8_t>> writes() const;

private:
    std::string id_;
    std::atomic_bool open_{};
    std::atomic_uint64_t sequence_{};
    std::atomic_uint64_t receivedBytes_{};
    std::atomic_uint64_t transmittedBytes_{};
    std::atomic_uint64_t receivedChunks_{};
    std::atomic_uint64_t transmittedChunks_{};
    mutable std::mutex writesMutex_;
    std::vector<std::vector<std::uint8_t>> writes_;
};

}  // namespace lab::core


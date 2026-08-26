#pragma once

#include "lab/core/data_source.hpp"

#include <QThread>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace lab::adapters::network {

enum class NetworkMode { TcpClient, TcpServer, Udp };

struct NetworkSettings {
    NetworkMode mode{NetworkMode::TcpClient};
    std::string remoteHost{"127.0.0.1"};
    std::uint16_t remotePort{9000};
    std::string bindAddress{"0.0.0.0"};
    std::uint16_t localPort{9000};
};

class NetworkWorker;

class NetworkSource final : public lab::core::IDataSource {
public:
    NetworkSource();
    ~NetworkSource() override;

    NetworkSource(const NetworkSource&) = delete;
    NetworkSource& operator=(const NetworkSource&) = delete;

    void setSettings(NetworkSettings settings);
    [[nodiscard]] NetworkSettings settings() const;

    bool open() override;
    void close() override;
    [[nodiscard]] bool isOpen() const noexcept override;
    bool write(std::span<const std::uint8_t> data) override;
    [[nodiscard]] std::string sourceId() const override;
    [[nodiscard]] lab::core::SourceStatistics statistics() const noexcept override;

private:
    void handleData(std::vector<std::uint8_t> payload, lab::core::Direction direction);

    mutable std::mutex settingsMutex_;
    NetworkSettings settings_;
    QThread ioThread_;
    NetworkWorker* worker_{};
    std::atomic_bool open_{};
    std::atomic_bool endpointCreated_{};
    std::atomic_uint64_t sequence_{};
    std::atomic_uint64_t receivedBytes_{};
    std::atomic_uint64_t transmittedBytes_{};
    std::atomic_uint64_t receivedChunks_{};
    std::atomic_uint64_t transmittedChunks_{};
    std::atomic_uint64_t errors_{};
};

[[nodiscard]] std::string toString(NetworkMode mode);

}  // namespace lab::adapters::network

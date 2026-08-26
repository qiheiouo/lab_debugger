#pragma once

#include "lab/core/data_source.hpp"

#include <QThread>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace lab::adapters::serial {

enum class Parity { None, Even, Odd, Space, Mark };
enum class StopBits { One, OneAndHalf, Two };
enum class FlowControl { None, Hardware, Software };

struct SerialSettings {
    std::string portName;
    std::int32_t baudRate{115200};
    std::int32_t dataBits{8};
    StopBits stopBits{StopBits::One};
    Parity parity{Parity::None};
    FlowControl flowControl{FlowControl::None};
};

struct PortInfo {
    std::string name;
    std::string description;
    std::string manufacturer;
    std::string serialNumber;
};

class SerialWorker;

class SerialSource final : public lab::core::IDataSource {
public:
    SerialSource();
    ~SerialSource() override;

    SerialSource(const SerialSource&) = delete;
    SerialSource& operator=(const SerialSource&) = delete;

    void setSettings(SerialSettings settings);
    [[nodiscard]] SerialSettings settings() const;
    [[nodiscard]] static std::vector<PortInfo> availablePorts();

    bool open() override;
    void close() override;
    [[nodiscard]] bool isOpen() const noexcept override;
    bool write(std::span<const std::uint8_t> data) override;
    [[nodiscard]] std::string sourceId() const override;
    [[nodiscard]] lab::core::SourceStatistics statistics() const noexcept override;

private:
    void handleData(std::vector<std::uint8_t> payload, lab::core::Direction direction);

    mutable std::mutex settingsMutex_;
    SerialSettings settings_;
    QThread ioThread_;
    SerialWorker* worker_{};
    std::atomic_bool open_{};
    std::atomic_uint64_t sequence_{};
    std::atomic_uint64_t receivedBytes_{};
    std::atomic_uint64_t transmittedBytes_{};
    std::atomic_uint64_t receivedChunks_{};
    std::atomic_uint64_t transmittedChunks_{};
    std::atomic_uint64_t errors_{};
};

}  // namespace lab::adapters::serial


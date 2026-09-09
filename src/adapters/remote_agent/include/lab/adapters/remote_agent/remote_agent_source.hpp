#pragma once

#include "lab/core/agent_protocol.hpp"
#include "lab/core/clock_sync.hpp"
#include "lab/core/data_source.hpp"

#include <QThread>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>

namespace lab::adapters::remote_agent {

struct RemoteAgentSettings {
    std::string host{"127.0.0.1"};
    std::uint16_t port{9750};
    std::string clientName{"Lab Debugger"};
    std::string clientVersion{"0.18.0"};
    bool autoReconnect{};
};

enum class HandshakeState { Disconnected, AwaitingHello, Ready, Error };

struct RemoteAgentCallbacks {
    std::function<void(const lab::core::agent::Hello&)> onHello;
    std::function<void(const lab::core::agent::TopicCatalog&)> onTopicCatalog;
    std::function<void(const lab::core::agent::SampleBatch&)> onSampleBatch;
    std::function<void(const lab::core::agent::DecodeIssue&)> onProtocolIssue;
    std::function<void(const lab::core::ClockSyncEstimate&)> onClockSync;
    std::function<void(const lab::core::agent::TopicFieldCatalog&)>
        onTopicFieldCatalog;
};

class RemoteAgentWorker;

class RemoteAgentSource final : public lab::core::IDataSource {
public:
    RemoteAgentSource();
    ~RemoteAgentSource() override;

    RemoteAgentSource(const RemoteAgentSource&) = delete;
    RemoteAgentSource& operator=(const RemoteAgentSource&) = delete;

    void setSettings(RemoteAgentSettings settings);
    [[nodiscard]] RemoteAgentSettings settings() const;
    void setAgentCallbacks(RemoteAgentCallbacks callbacks);

    bool open() override;
    void close() override;
    [[nodiscard]] bool isOpen() const noexcept override;
    bool write(std::span<const std::uint8_t> data) override;
    [[nodiscard]] std::string sourceId() const override;
    [[nodiscard]] lab::core::SourceStatistics statistics() const noexcept override;

    bool requestTopicCatalog();
    bool subscribe(const lab::core::agent::SubscriptionRequest& request);
    bool unsubscribe(const lab::core::agent::SubscriptionRequest& request);
    [[nodiscard]] HandshakeState handshakeState() const noexcept;
    [[nodiscard]] std::optional<lab::core::ClockSyncEstimate>
    clockSyncEstimate() const;

private:
    void handleHello(const lab::core::agent::Hello& hello);
    void handleSample(
        const lab::core::agent::Frame& frame,
        const lab::core::agent::SampleBatch& batch);
    void handleProtocolIssue(const lab::core::agent::DecodeIssue& issue);
    void handleClockSync(const lab::core::ClockSyncEstimate& estimate);
    void handleWireBytes(lab::core::Direction direction, std::size_t count);
    [[nodiscard]] RemoteAgentCallbacks agentCallbacks() const;
    [[nodiscard]] std::string agentSourceId(const std::string& topic = {}) const;

    mutable std::mutex settingsMutex_;
    RemoteAgentSettings settings_;
    mutable std::mutex agentCallbackMutex_;
    RemoteAgentCallbacks agentCallbacks_;
    mutable std::mutex agentIdentityMutex_;
    std::string agentId_;
    mutable std::mutex clockSyncMutex_;
    std::optional<lab::core::ClockSyncEstimate> clockSyncEstimate_;
    QThread ioThread_;
    RemoteAgentWorker* worker_{};
    std::atomic_bool open_{};
    std::atomic_bool endpointCreated_{};
    std::atomic<HandshakeState> handshakeState_{HandshakeState::Disconnected};
    std::atomic_uint64_t receivedBytes_{};
    std::atomic_uint64_t transmittedBytes_{};
    std::atomic_uint64_t receivedChunks_{};
    std::atomic_uint64_t transmittedChunks_{};
    std::atomic_uint64_t errors_{};
};

[[nodiscard]] std::string toString(HandshakeState state);

}  // namespace lab::adapters::remote_agent

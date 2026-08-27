#pragma once

#include "lab/core/remote_agent_session.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

namespace lab_debug_agent {

struct TcpServerSettings {
    std::string bindAddress{"127.0.0.1"};
    std::uint16_t port{9750};
};

struct TcpServerCallbacks {
    std::function<lab::core::agent::TopicCatalog()> onCatalogRequested;
    std::function<std::optional<std::string>(
        const lab::core::agent::SubscriptionRequest&)> onSubscribe;
    std::function<std::optional<std::string>(
        const lab::core::agent::SubscriptionRequest&)> onUnsubscribe;
    std::function<void()> onClientDisconnected;
    std::function<void(const std::string&)> onInfo;
    std::function<void(const std::string&)> onWarning;
};

class AgentTcpServer final {
public:
    AgentTcpServer(
        TcpServerSettings settings,
        lab::core::agent::Hello identity,
        TcpServerCallbacks callbacks);
    ~AgentTcpServer();

    AgentTcpServer(const AgentTcpServer&) = delete;
    AgentTcpServer& operator=(const AgentTcpServer&) = delete;

    bool start(std::string* error = nullptr);
    void stop();

    bool publishCatalog(const lab::core::agent::TopicCatalog& catalog);
    bool publishSample(
        const lab::core::agent::SampleBatch& sample,
        lab::core::Timestamp sourceTimestamp,
        lab::core::Timestamp agentReceiveTimestamp);
    bool publishError(const lab::core::agent::AgentError& error);
    bool publishPing(std::uint64_t nonce);

    [[nodiscard]] bool clientReady() const noexcept;
    [[nodiscard]] std::uint32_t negotiatedCapabilities() const noexcept;

private:
    void run(std::stop_token stopToken);
    void serveClient(int client, std::stop_token stopToken);
    bool sendEncoded(const std::vector<std::uint8_t>& bytes);
    void closeListener();
    void closeClient();
    void processAction(const lab::core::agent::ServerAction& action);
    void info(const std::string& message) const;
    void warning(const std::string& message) const;

    TcpServerSettings settings_;
    lab::core::agent::ServerSession session_;
    TcpServerCallbacks callbacks_;
    std::jthread thread_;
    std::atomic_int listener_{-1};
    std::atomic_int client_{-1};
    // Keep outbound sequence allocation and the corresponding socket write in
    // one critical section. ServerSession protects its own state, but without
    // this lock concurrent ROS callbacks could allocate N and N+1 and then
    // write them to the TCP stream in the opposite order.
    std::mutex sessionIoMutex_;
    std::mutex sendMutex_;
};

}  // namespace lab_debug_agent

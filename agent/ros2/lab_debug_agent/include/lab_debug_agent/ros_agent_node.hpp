#pragma once

#include "lab_debug_agent/tcp_server.hpp"

#include <rclcpp/generic_subscription.hpp>
#include <rclcpp/rclcpp.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace lab_debug_agent {

class RosAgentNode final : public rclcpp::Node {
public:
    RosAgentNode();
    ~RosAgentNode() override;

private:
    struct ActiveSubscription {
        lab::core::agent::SubscriptionRequest request;
        std::shared_ptr<rclcpp::GenericSubscription> subscription;
    };

    [[nodiscard]] lab::core::agent::TopicCatalog buildCatalog();
    [[nodiscard]] std::optional<std::string> subscribeTopic(
        const lab::core::agent::SubscriptionRequest& request);
    [[nodiscard]] std::optional<std::string> unsubscribeTopic(
        const lab::core::agent::SubscriptionRequest& request);
    void handleSerializedMessage(
        const std::string& topic,
        const std::string& type,
        const std::shared_ptr<rclcpp::SerializedMessage>& message);
    void clearSubscriptions();
    void refreshGraph();
    void heartbeat();

    std::unique_ptr<AgentTcpServer> server_;
    std::mutex subscriptionsMutex_;
    std::unordered_map<std::string, ActiveSubscription> subscriptions_;
    std::mutex catalogMutex_;
    std::string catalogSignature_;
    std::uint64_t catalogRevision_{};
    bool catalogInitialized_{};
    std::atomic_uint64_t lastSentRevision_{};
    std::atomic_uint64_t heartbeatNonce_{};
    rclcpp::TimerBase::SharedPtr graphTimer_;
    rclcpp::TimerBase::SharedPtr heartbeatTimer_;
};

}  // namespace lab_debug_agent

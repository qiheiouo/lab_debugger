#include "lab_debug_agent/ros_agent_node.hpp"

#include "lab_debug_agent/field_mapper.hpp"
#include "lab/core/timestamp.hpp"

#include <rmw/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace lab_debug_agent {
namespace {

constexpr std::uint32_t agentCapabilities =
    lab::core::agent::capabilityMask(lab::core::agent::Capability::TopicDiscovery) |
    lab::core::agent::capabilityMask(lab::core::agent::Capability::SerializedMessages) |
    lab::core::agent::capabilityMask(lab::core::agent::Capability::NumericFields) |
    lab::core::agent::capabilityMask(lab::core::agent::Capability::TextFields) |
    lab::core::agent::capabilityMask(lab::core::agent::Capability::GraphUpdates);

std::string defaultHostName() {
    std::array<char, 256> value{};
    if (::gethostname(value.data(), value.size() - 1) == 0) return value.data();
    return "unknown-host";
}

lab::core::agent::Reliability reliability(rmw_qos_reliability_policy_t value) {
    switch (value) {
    case RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT:
        return lab::core::agent::Reliability::BestEffort;
    case RMW_QOS_POLICY_RELIABILITY_RELIABLE:
        return lab::core::agent::Reliability::Reliable;
    default: return lab::core::agent::Reliability::Unknown;
    }
}

lab::core::agent::Durability durability(rmw_qos_durability_policy_t value) {
    switch (value) {
    case RMW_QOS_POLICY_DURABILITY_VOLATILE:
        return lab::core::agent::Durability::Volatile;
    case RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL:
        return lab::core::agent::Durability::TransientLocal;
    default: return lab::core::agent::Durability::Unknown;
    }
}

template<typename Value>
Value commonPolicy(const std::vector<Value>& values, Value unknown) {
    if (values.empty()) return unknown;
    return std::all_of(values.begin(), values.end(), [&](Value value) {
               return value == values.front();
           })
               ? values.front()
               : unknown;
}

}  // namespace

RosAgentNode::RosAgentNode() : rclcpp::Node("lab_debug_agent") {
    const auto bindAddress = declare_parameter<std::string>("bind_address", "127.0.0.1");
    const auto portValue = declare_parameter<std::int64_t>("port", 9750);
    const auto agentId = declare_parameter<std::string>("agent_id", get_name());
    if (portValue <= 0 || portValue > 65535) {
        throw std::invalid_argument("port must be in range 1..65535");
    }
    if (agentId.empty()) {
        throw std::invalid_argument("agent_id must not be empty");
    }

    TcpServerCallbacks callbacks;
    callbacks.onCatalogRequested = [this] {
        auto catalog = buildCatalog();
        lastSentRevision_.store(catalog.graphRevision);
        return catalog;
    };
    callbacks.onSubscribe = [this](const auto& request) {
        return subscribeTopic(request);
    };
    callbacks.onUnsubscribe = [this](const auto& request) {
        return unsubscribeTopic(request);
    };
    callbacks.onClientDisconnected = [this] { clearSubscriptions(); };
    callbacks.onInfo = [this](const std::string& message) {
        RCLCPP_INFO(get_logger(), "%s", message.c_str());
    };
    callbacks.onWarning = [this](const std::string& message) {
        RCLCPP_WARN(get_logger(), "%s", message.c_str());
    };

    server_ = std::make_unique<AgentTcpServer>(
        TcpServerSettings{bindAddress, static_cast<std::uint16_t>(portValue)},
        lab::core::agent::Hello{agentId, "0.1.0", defaultHostName(), agentCapabilities},
        std::move(callbacks));
    std::string error;
    if (!server_->start(&error)) throw std::runtime_error(error);

    graphTimer_ = create_wall_timer(
        std::chrono::seconds(1), [this] { refreshGraph(); });
    heartbeatTimer_ = create_wall_timer(
        std::chrono::seconds(2), [this] { heartbeat(); });
}

RosAgentNode::~RosAgentNode() {
    if (server_) server_->stop();
    clearSubscriptions();
}

lab::core::agent::TopicCatalog RosAgentNode::buildCatalog() {
    std::vector<lab::core::agent::TopicDescriptor> topics;
    const auto graph = get_topic_names_and_types();
    for (const auto& [name, types] : graph) {
        const auto endpoints = get_publishers_info_by_topic(name);
        std::vector<lab::core::agent::Reliability> reliabilities;
        std::vector<lab::core::agent::Durability> durabilities;
        reliabilities.reserve(endpoints.size());
        durabilities.reserve(endpoints.size());
        for (const auto& endpoint : endpoints) {
            const auto& qos = endpoint.qos_profile().get_rmw_qos_profile();
            reliabilities.push_back(reliability(qos.reliability));
            durabilities.push_back(durability(qos.durability));
        }
        const auto topicReliability = commonPolicy(
            reliabilities, lab::core::agent::Reliability::Unknown);
        const auto topicDurability = commonPolicy(
            durabilities, lab::core::agent::Durability::Unknown);
        for (const auto& type : types) {
            topics.push_back({name, type, topicReliability, topicDurability});
        }
    }
    std::sort(topics.begin(), topics.end(), [](const auto& left, const auto& right) {
        return std::tie(left.name, left.type) < std::tie(right.name, right.type);
    });

    std::ostringstream signature;
    for (const auto& topic : topics) {
        signature << topic.name << '\0' << topic.type << '\0'
                  << static_cast<int>(topic.reliability) << '\0'
                  << static_cast<int>(topic.durability) << '\n';
    }
    std::scoped_lock lock(catalogMutex_);
    if (signature.str() != catalogSignature_) {
        catalogSignature_ = signature.str();
        ++catalogRevision_;
    }
    return {catalogRevision_, std::move(topics)};
}

std::optional<std::string> RosAgentNode::subscribeTopic(
    const lab::core::agent::SubscriptionRequest& request) {
    const auto graph = get_topic_names_and_types();
    const auto topic = graph.find(request.topic);
    if (topic == graph.end() ||
        std::find(topic->second.begin(), topic->second.end(), request.type) ==
            topic->second.end()) {
        return "Topic/type is not present in the ROS graph";
    }

    try {
        rclcpp::QoS qos(rclcpp::KeepLast(request.queueDepth));
        if (request.reliability == lab::core::agent::Reliability::Reliable) {
            qos.reliable();
        } else {
            qos.best_effort();
        }
        qos.durability_volatile();
        auto subscription = create_generic_subscription(
            request.topic,
            request.type,
            qos,
            [this, topicName = request.topic, type = request.type](
                std::shared_ptr<rclcpp::SerializedMessage> message) {
                handleSerializedMessage(topicName, type, message);
            });
        std::scoped_lock lock(subscriptionsMutex_);
        subscriptions_[request.topic] = std::move(subscription);
        RCLCPP_INFO(
            get_logger(), "Subscribed to %s [%s]", request.topic.c_str(), request.type.c_str());
        return std::nullopt;
    } catch (const std::exception& exception) {
        return std::string("Failed to create GenericSubscription: ") + exception.what();
    }
}

std::optional<std::string> RosAgentNode::unsubscribeTopic(
    const lab::core::agent::SubscriptionRequest& request) {
    std::scoped_lock lock(subscriptionsMutex_);
    const auto erased = subscriptions_.erase(request.topic);
    if (erased == 0) return "Topic is not currently subscribed";
    RCLCPP_INFO(get_logger(), "Unsubscribed from %s", request.topic.c_str());
    return std::nullopt;
}

void RosAgentNode::handleSerializedMessage(
    const std::string& topic,
    const std::string& type,
    const std::shared_ptr<rclcpp::SerializedMessage>& message) {
    if (!server_ || !message) return;
    const auto received = lab::core::nowTimestampNs();
    const auto& raw = message->get_rcl_serialized_message();
    std::vector<std::uint8_t> bytes;
    if (raw.buffer && raw.buffer_length > 0) {
        bytes.assign(raw.buffer, raw.buffer + raw.buffer_length);
    }
    auto mapped = mapSerializedFields(type, *message);
    if (!mapped.warning.empty()) {
        RCLCPP_WARN_THROTTLE(
            get_logger(), *get_clock(), 5000, "%s", mapped.warning.c_str());
    }
    server_->publishSample(
        {topic, type, std::move(bytes), std::move(mapped.fields)},
        mapped.sourceTimestamp,
        received);
}

void RosAgentNode::clearSubscriptions() {
    std::scoped_lock lock(subscriptionsMutex_);
    subscriptions_.clear();
}

void RosAgentNode::refreshGraph() {
    if (!server_ || !server_->clientReady() ||
        (server_->negotiatedCapabilities() &
         lab::core::agent::capabilityMask(
             lab::core::agent::Capability::GraphUpdates)) == 0U) {
        return;
    }
    auto catalog = buildCatalog();
    if (catalog.graphRevision != lastSentRevision_.load() &&
        server_->publishCatalog(catalog)) {
        lastSentRevision_.store(catalog.graphRevision);
    }
}

void RosAgentNode::heartbeat() {
    if (server_ && server_->clientReady()) {
        server_->publishPing(heartbeatNonce_.fetch_add(1));
    }
}

}  // namespace lab_debug_agent

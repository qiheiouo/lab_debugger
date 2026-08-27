#pragma once

#include "lab/core/agent_protocol.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace lab::core::agent {

enum class ServerSessionState { Idle, AwaitingHelloAck, Ready, Error };

struct CatalogRequestAction {
    bool operator==(const CatalogRequestAction&) const = default;
};

struct SubscribeAction {
    SubscriptionRequest request;
    bool operator==(const SubscribeAction&) const = default;
};

struct UnsubscribeAction {
    SubscriptionRequest request;
    bool operator==(const UnsubscribeAction&) const = default;
};

struct PeerErrorAction {
    AgentError error;
    bool operator==(const PeerErrorAction&) const = default;
};

using ServerAction = std::variant<
    CatalogRequestAction,
    SubscribeAction,
    UnsubscribeAction,
    PeerErrorAction>;

struct ServerConsumeResult {
    std::vector<std::vector<std::uint8_t>> outboundFrames;
    std::vector<ServerAction> actions;
    std::vector<DecodeIssue> decodeIssues;
    std::optional<std::string> fatalError;
};

class ServerSession {
public:
    explicit ServerSession(Hello identity);

    [[nodiscard]] std::vector<std::uint8_t> start();
    [[nodiscard]] ServerConsumeResult consume(std::span<const std::uint8_t> bytes);
    void reset();

    [[nodiscard]] std::optional<std::vector<std::uint8_t>> makeTopicCatalog(
        const TopicCatalog& catalog);
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> makeTopicFieldCatalog(
        const TopicFieldCatalog& catalog);
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> makeSample(
        const SampleBatch& sample,
        Timestamp sourceTimestamp,
        Timestamp agentReceiveTimestamp);
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> makeError(
        const AgentError& error);
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> makePing(
        std::uint64_t nonce);

    [[nodiscard]] ServerSessionState state() const noexcept;
    [[nodiscard]] std::optional<HelloAck> clientIdentity() const;
    [[nodiscard]] std::uint32_t negotiatedCapabilities() const noexcept;

private:
    [[nodiscard]] std::vector<std::uint8_t> encodeLocked(
        MessageType type,
        std::vector<std::uint8_t> payload,
        Timestamp sourceTimestamp,
        Timestamp agentReceiveTimestamp);
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> encodeReadyLocked(
        MessageType type,
        std::vector<std::uint8_t> payload,
        Timestamp sourceTimestamp,
        Timestamp agentReceiveTimestamp);
    void failLocked(ServerConsumeResult& result, std::string message);

    Hello identity_;
    mutable std::mutex mutex_;
    StreamDecoder decoder_;
    ServerSessionState state_{ServerSessionState::Idle};
    std::optional<std::uint64_t> lastInboundSequence_;
    std::uint64_t outboundSequence_{};
    std::optional<HelloAck> clientIdentity_;
    std::uint32_t negotiatedCapabilities_{};
};

[[nodiscard]] std::string toString(ServerSessionState state);

}  // namespace lab::core::agent

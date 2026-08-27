#include "lab/core/remote_agent_session.hpp"

#include "lab/core/timestamp.hpp"

#include <algorithm>
#include <utility>

namespace lab::core::agent {
namespace {

constexpr std::uint32_t sampleCapabilityMask =
    capabilityMask(Capability::SerializedMessages) |
    capabilityMask(Capability::NumericFields) |
    capabilityMask(Capability::TextFields);

bool hasCapability(std::uint32_t capabilities, Capability capability) {
    return (capabilities & capabilityMask(capability)) != 0U;
}

}  // namespace

ServerSession::ServerSession(Hello identity) : identity_(std::move(identity)) {}

std::vector<std::uint8_t> ServerSession::start() {
    std::scoped_lock lock(mutex_);
    decoder_.reset();
    lastInboundSequence_.reset();
    outboundSequence_ = 0;
    clientIdentity_.reset();
    negotiatedCapabilities_ = 0;
    state_ = ServerSessionState::AwaitingHelloAck;
    const auto now = nowTimestampNs();
    return encodeLocked(MessageType::Hello, encodeHello(identity_), now, now);
}

ServerConsumeResult ServerSession::consume(std::span<const std::uint8_t> bytes) {
    std::scoped_lock lock(mutex_);
    ServerConsumeResult result;
    if (state_ == ServerSessionState::Idle || state_ == ServerSessionState::Error) {
        result.fatalError = "Agent server session is not accepting input";
        return result;
    }

    auto decoded = decoder_.consume(bytes);
    result.decodeIssues = std::move(decoded.issues);
    for (const auto& frame : decoded.frames) {
        if (frame.flags != 0) {
            failLocked(result, "Client sent unsupported non-zero frame flags");
            break;
        }
        if (lastInboundSequence_ && frame.sequence <= *lastInboundSequence_) {
            failLocked(result, "Client frame sequence is not strictly increasing");
            break;
        }
        lastInboundSequence_ = frame.sequence;

        std::string error;
        if (state_ == ServerSessionState::AwaitingHelloAck) {
            if (frame.type != MessageType::HelloAck) {
                failLocked(result, "Client must send hello_ack as its first frame");
                break;
            }
            const auto ack = decodeHelloAck(frame.payload, &error);
            if (!ack) {
                failLocked(result, "Malformed hello_ack payload: " + error);
                break;
            }
            if ((ack->requestedCapabilities & ~identity_.capabilities) != 0U) {
                failLocked(result, "Client requested capabilities not offered by Agent");
                break;
            }
            if (hasCapability(
                    ack->requestedCapabilities, Capability::GraphUpdates) &&
                !hasCapability(
                    ack->requestedCapabilities, Capability::TopicDiscovery)) {
                failLocked(
                    result,
                    "Graph updates require the topic discovery capability");
                break;
            }
            if (hasCapability(
                    ack->requestedCapabilities,
                    Capability::TopicFieldCapabilities) &&
                !hasCapability(
                    ack->requestedCapabilities, Capability::TopicDiscovery)) {
                failLocked(
                    result,
                    "Topic field capabilities require the topic discovery capability");
                break;
            }
            clientIdentity_ = *ack;
            negotiatedCapabilities_ = ack->requestedCapabilities;
            state_ = ServerSessionState::Ready;
            continue;
        }

        switch (frame.type) {
        case MessageType::TopicCatalogRequest:
            if (!hasCapability(
                    negotiatedCapabilities_, Capability::TopicDiscovery)) {
                failLocked(
                    result,
                    "Client requested a topic catalog without negotiating topic discovery");
            } else if (!frame.payload.empty()) {
                failLocked(result, "Topic catalog request payload must be empty");
            } else {
                result.actions.emplace_back(CatalogRequestAction{});
            }
            break;
        case MessageType::Subscribe: {
            if ((negotiatedCapabilities_ & sampleCapabilityMask) == 0U) {
                failLocked(
                    result,
                    "Client subscribed without negotiating any sample capability");
                break;
            }
            const auto request = decodeSubscriptionRequest(frame.payload, &error);
            if (!request) {
                failLocked(result, "Malformed subscribe payload: " + error);
            } else {
                result.actions.emplace_back(SubscribeAction{*request});
            }
            break;
        }
        case MessageType::Unsubscribe: {
            if ((negotiatedCapabilities_ & sampleCapabilityMask) == 0U) {
                failLocked(
                    result,
                    "Client unsubscribed without negotiating any sample capability");
                break;
            }
            const auto request = decodeSubscriptionRequest(frame.payload, &error);
            if (!request) {
                failLocked(result, "Malformed unsubscribe payload: " + error);
            } else {
                result.actions.emplace_back(UnsubscribeAction{*request});
            }
            break;
        }
        case MessageType::Ping: {
            const auto nonce = decodeNonce(frame.payload, &error);
            if (!nonce) {
                failLocked(result, "Malformed ping payload: " + error);
            } else {
                const auto now = nowTimestampNs();
                result.outboundFrames.push_back(encodeLocked(
                    MessageType::Pong, encodeNonce(*nonce), now, now));
            }
            break;
        }
        case MessageType::Pong:
            if (!decodeNonce(frame.payload, &error)) {
                failLocked(result, "Malformed pong payload: " + error);
            }
            break;
        case MessageType::Error: {
            const auto peerError = decodeAgentError(frame.payload, &error);
            if (!peerError) {
                failLocked(result, "Malformed client error payload: " + error);
            } else {
                result.actions.emplace_back(PeerErrorAction{*peerError});
            }
            break;
        }
        case MessageType::Hello:
        case MessageType::HelloAck:
        case MessageType::TopicCatalog:
        case MessageType::TopicFieldCatalog:
        case MessageType::SampleBatch:
            failLocked(
                result,
                "Client sent a message invalid for the ready state: " +
                    toString(frame.type));
            break;
        }
        if (result.fatalError) break;
    }
    return result;
}

void ServerSession::reset() {
    std::scoped_lock lock(mutex_);
    decoder_.reset();
    state_ = ServerSessionState::Idle;
    lastInboundSequence_.reset();
    outboundSequence_ = 0;
    clientIdentity_.reset();
    negotiatedCapabilities_ = 0;
}

std::optional<std::vector<std::uint8_t>> ServerSession::makeTopicCatalog(
    const TopicCatalog& catalog) {
    std::scoped_lock lock(mutex_);
    if (state_ != ServerSessionState::Ready ||
        !hasCapability(negotiatedCapabilities_, Capability::TopicDiscovery)) {
        return std::nullopt;
    }
    const auto now = nowTimestampNs();
    return encodeReadyLocked(
        MessageType::TopicCatalog, encodeTopicCatalog(catalog), now, now);
}

std::optional<std::vector<std::uint8_t>> ServerSession::makeTopicFieldCatalog(
    const TopicFieldCatalog& catalog) {
    std::scoped_lock lock(mutex_);
    if (state_ != ServerSessionState::Ready ||
        !hasCapability(
            negotiatedCapabilities_, Capability::TopicFieldCapabilities)) {
        return std::nullopt;
    }
    const auto now = nowTimestampNs();
    return encodeReadyLocked(
        MessageType::TopicFieldCatalog,
        encodeTopicFieldCatalog(catalog),
        now,
        now);
}

std::optional<std::vector<std::uint8_t>> ServerSession::makeSample(
    const SampleBatch& sample,
    Timestamp sourceTimestamp,
    Timestamp agentReceiveTimestamp) {
    std::scoped_lock lock(mutex_);
    if (state_ != ServerSessionState::Ready ||
        (negotiatedCapabilities_ & sampleCapabilityMask) == 0U) {
        return std::nullopt;
    }
    auto filtered = sample;
    if (!hasCapability(
            negotiatedCapabilities_, Capability::SerializedMessages)) {
        filtered.serializedData.clear();
    }
    const auto numericFields = hasCapability(
        negotiatedCapabilities_, Capability::NumericFields);
    const auto textFields = hasCapability(
        negotiatedCapabilities_, Capability::TextFields);
    std::erase_if(filtered.fields, [&](const FieldValue& field) {
        return std::holds_alternative<std::string>(field.value)
                   ? !textFields
                   : !numericFields;
    });
    if (filtered.serializedData.empty() && filtered.fields.empty()) {
        return std::nullopt;
    }
    if (agentReceiveTimestamp == 0) agentReceiveTimestamp = nowTimestampNs();
    if (sourceTimestamp == 0) sourceTimestamp = agentReceiveTimestamp;
    return encodeReadyLocked(
        MessageType::SampleBatch,
        encodeSampleBatch(filtered),
        sourceTimestamp,
        agentReceiveTimestamp);
}

std::optional<std::vector<std::uint8_t>> ServerSession::makeError(
    const AgentError& error) {
    std::scoped_lock lock(mutex_);
    if (state_ != ServerSessionState::Ready) return std::nullopt;
    const auto now = nowTimestampNs();
    return encodeReadyLocked(MessageType::Error, encodeAgentError(error), now, now);
}

std::optional<std::vector<std::uint8_t>> ServerSession::makePing(
    std::uint64_t nonce) {
    std::scoped_lock lock(mutex_);
    if (state_ != ServerSessionState::Ready) return std::nullopt;
    const auto now = nowTimestampNs();
    return encodeReadyLocked(MessageType::Ping, encodeNonce(nonce), now, now);
}

ServerSessionState ServerSession::state() const noexcept {
    std::scoped_lock lock(mutex_);
    return state_;
}

std::optional<HelloAck> ServerSession::clientIdentity() const {
    std::scoped_lock lock(mutex_);
    return clientIdentity_;
}

std::uint32_t ServerSession::negotiatedCapabilities() const noexcept {
    std::scoped_lock lock(mutex_);
    return negotiatedCapabilities_;
}

std::vector<std::uint8_t> ServerSession::encodeLocked(
    MessageType type,
    std::vector<std::uint8_t> payload,
    Timestamp sourceTimestamp,
    Timestamp agentReceiveTimestamp) {
    return encodeFrame({type,
                        0,
                        outboundSequence_++,
                        sourceTimestamp,
                        agentReceiveTimestamp,
                        std::move(payload)});
}

std::optional<std::vector<std::uint8_t>> ServerSession::encodeReadyLocked(
    MessageType type,
    std::vector<std::uint8_t> payload,
    Timestamp sourceTimestamp,
    Timestamp agentReceiveTimestamp) {
    if (state_ != ServerSessionState::Ready) return std::nullopt;
    return encodeLocked(
        type, std::move(payload), sourceTimestamp, agentReceiveTimestamp);
}

void ServerSession::failLocked(ServerConsumeResult& result, std::string message) {
    if (result.fatalError) return;
    result.fatalError = message;
    const auto now = nowTimestampNs();
    result.outboundFrames.push_back(encodeLocked(
        MessageType::Error,
        encodeAgentError({1001U, "protocol", message}),
        now,
        now));
    state_ = ServerSessionState::Error;
}

std::string toString(ServerSessionState state) {
    switch (state) {
    case ServerSessionState::Idle: return "idle";
    case ServerSessionState::AwaitingHelloAck: return "awaiting_hello_ack";
    case ServerSessionState::Ready: return "ready";
    case ServerSessionState::Error: return "error";
    }
    return "unknown";
}

}  // namespace lab::core::agent

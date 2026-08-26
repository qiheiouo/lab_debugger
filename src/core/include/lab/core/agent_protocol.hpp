#pragma once

#include "lab/core/timestamp.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace lab::core::agent {

inline constexpr std::uint8_t protocolVersion = 1;
inline constexpr std::size_t frameHeaderSize = 40;
inline constexpr std::uint32_t maximumPayloadBytes = 16U * 1024U * 1024U;

enum class MessageType : std::uint8_t {
    Hello = 1,
    HelloAck = 2,
    TopicCatalog = 3,
    Subscribe = 4,
    Unsubscribe = 5,
    SampleBatch = 6,
    Error = 7,
    Ping = 8,
    Pong = 9,
    TopicCatalogRequest = 10
};

enum class Capability : std::uint32_t {
    TopicDiscovery = 1U << 0U,
    SerializedMessages = 1U << 1U,
    NumericFields = 1U << 2U,
    TextFields = 1U << 3U,
    GraphUpdates = 1U << 4U
};

[[nodiscard]] constexpr std::uint32_t capabilityMask(Capability capability) noexcept {
    return static_cast<std::uint32_t>(capability);
}

enum class Reliability : std::uint8_t { Unknown = 0, BestEffort = 1, Reliable = 2 };
enum class Durability : std::uint8_t { Unknown = 0, Volatile = 1, TransientLocal = 2 };

struct Frame {
    MessageType type{MessageType::Ping};
    std::uint16_t flags{};
    std::uint64_t sequence{};
    Timestamp sourceTimestamp{};
    Timestamp agentReceiveTimestamp{};
    std::vector<std::uint8_t> payload;

    bool operator==(const Frame&) const = default;
};

struct Hello {
    std::string agentId;
    std::string softwareVersion;
    std::string hostName;
    std::uint32_t capabilities{};

    bool operator==(const Hello&) const = default;
};

struct HelloAck {
    std::string clientName;
    std::string softwareVersion;
    std::uint32_t requestedCapabilities{};

    bool operator==(const HelloAck&) const = default;
};

struct TopicDescriptor {
    std::string name;
    std::string type;
    Reliability reliability{Reliability::Unknown};
    Durability durability{Durability::Unknown};

    bool operator==(const TopicDescriptor&) const = default;
};

struct TopicCatalog {
    std::uint64_t graphRevision{};
    std::vector<TopicDescriptor> topics;

    bool operator==(const TopicCatalog&) const = default;
};

struct SubscriptionRequest {
    std::uint64_t requestId{};
    std::string topic;
    std::string type;
    Reliability reliability{Reliability::Unknown};
    std::uint32_t queueDepth{10};

    bool operator==(const SubscriptionRequest&) const = default;
};

using FieldData = std::variant<double, bool, std::string>;

struct FieldValue {
    std::string path;
    std::string unit;
    FieldData value{};

    bool operator==(const FieldValue&) const = default;
};

struct SampleBatch {
    std::string topic;
    std::string type;
    std::vector<std::uint8_t> serializedData;
    std::vector<FieldValue> fields;

    bool operator==(const SampleBatch&) const = default;
};

struct AgentError {
    std::uint32_t code{};
    std::string context;
    std::string message;

    bool operator==(const AgentError&) const = default;
};

enum class DecodeIssueKind {
    Garbage,
    UnsupportedVersion,
    UnknownMessageType,
    OversizedPayload,
    ChecksumMismatch
};

struct DecodeIssue {
    DecodeIssueKind kind{DecodeIssueKind::Garbage};
    std::size_t discardedBytes{};
    std::string message;
};

struct DecodeResult {
    std::vector<Frame> frames;
    std::vector<DecodeIssue> issues;
};

struct StreamStatistics {
    std::uint64_t decodedFrames{};
    std::uint64_t discardedBytes{};
    std::uint64_t unsupportedVersions{};
    std::uint64_t unknownMessageTypes{};
    std::uint64_t oversizedPayloads{};
    std::uint64_t checksumErrors{};
};

class StreamDecoder {
public:
    [[nodiscard]] DecodeResult consume(std::span<const std::uint8_t> bytes);
    void reset();

    [[nodiscard]] std::size_t bufferedBytes() const noexcept;
    [[nodiscard]] const StreamStatistics& statistics() const noexcept;

private:
    std::vector<std::uint8_t> buffer_;
    StreamStatistics statistics_;
};

[[nodiscard]] bool isKnownMessageType(std::uint8_t value) noexcept;
[[nodiscard]] std::string toString(MessageType type);
[[nodiscard]] std::uint32_t crc32Ieee(std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] std::vector<std::uint8_t> encodeFrame(const Frame& frame);

[[nodiscard]] std::vector<std::uint8_t> encodeHello(const Hello& value);
[[nodiscard]] std::optional<Hello> decodeHello(
    std::span<const std::uint8_t> payload,
    std::string* error = nullptr);

[[nodiscard]] std::vector<std::uint8_t> encodeHelloAck(const HelloAck& value);
[[nodiscard]] std::optional<HelloAck> decodeHelloAck(
    std::span<const std::uint8_t> payload,
    std::string* error = nullptr);

[[nodiscard]] std::vector<std::uint8_t> encodeTopicCatalog(const TopicCatalog& value);
[[nodiscard]] std::optional<TopicCatalog> decodeTopicCatalog(
    std::span<const std::uint8_t> payload,
    std::string* error = nullptr);

[[nodiscard]] std::vector<std::uint8_t> encodeSubscriptionRequest(
    const SubscriptionRequest& value);
[[nodiscard]] std::optional<SubscriptionRequest> decodeSubscriptionRequest(
    std::span<const std::uint8_t> payload,
    std::string* error = nullptr);

[[nodiscard]] std::vector<std::uint8_t> encodeSampleBatch(const SampleBatch& value);
[[nodiscard]] std::optional<SampleBatch> decodeSampleBatch(
    std::span<const std::uint8_t> payload,
    std::string* error = nullptr);

[[nodiscard]] std::vector<std::uint8_t> encodeAgentError(const AgentError& value);
[[nodiscard]] std::optional<AgentError> decodeAgentError(
    std::span<const std::uint8_t> payload,
    std::string* error = nullptr);

[[nodiscard]] std::vector<std::uint8_t> encodeNonce(std::uint64_t nonce);
[[nodiscard]] std::optional<std::uint64_t> decodeNonce(
    std::span<const std::uint8_t> payload,
    std::string* error = nullptr);

}  // namespace lab::core::agent

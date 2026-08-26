#include "lab/core/agent_protocol.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <stdexcept>
#include <utility>

namespace lab::core::agent {
namespace {

constexpr std::array<std::uint8_t, 4> magic{'L', 'D', 'R', 'A'};
constexpr std::size_t maximumStringBytes = 1024U * 1024U;
constexpr std::size_t maximumCollectionItems = 100'000U;

void appendU8(std::vector<std::uint8_t>& output, std::uint8_t value) {
    output.push_back(value);
}

void appendU16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void appendU32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned int shift = 0; shift < 32U; shift += 8U) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

void appendU64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    for (unsigned int shift = 0; shift < 64U; shift += 8U) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

std::uint16_t readU16(std::span<const std::uint8_t> data, std::size_t offset) {
    return static_cast<std::uint16_t>(data[offset]) |
           static_cast<std::uint16_t>(data[offset + 1]) << 8U;
}

std::uint32_t readU32(std::span<const std::uint8_t> data, std::size_t offset) {
    std::uint32_t result = 0;
    for (unsigned int index = 0; index < 4U; ++index) {
        result |= static_cast<std::uint32_t>(data[offset + index]) << (index * 8U);
    }
    return result;
}

std::uint64_t readU64(std::span<const std::uint8_t> data, std::size_t offset) {
    std::uint64_t result = 0;
    for (unsigned int index = 0; index < 8U; ++index) {
        result |= static_cast<std::uint64_t>(data[offset + index]) << (index * 8U);
    }
    return result;
}

std::uint32_t updateCrc32(
    std::uint32_t crc,
    std::span<const std::uint8_t> bytes) noexcept {
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) != 0U ? (crc >> 1U) ^ 0xEDB88320U : crc >> 1U;
        }
    }
    return crc;
}

std::uint32_t frameCrc(
    std::span<const std::uint8_t> headerWithoutCrc,
    std::span<const std::uint8_t> payload) noexcept {
    auto crc = updateCrc32(0xFFFFFFFFU, headerWithoutCrc);
    crc = updateCrc32(crc, payload);
    return ~crc;
}

std::size_t magicPrefixSuffixLength(std::span<const std::uint8_t> bytes) {
    const auto maximum = std::min(bytes.size(), magic.size() - 1U);
    for (auto count = maximum; count > 0; --count) {
        if (std::equal(bytes.end() - static_cast<std::ptrdiff_t>(count),
                       bytes.end(), magic.begin())) {
            return count;
        }
    }
    return 0;
}

class PayloadWriter {
public:
    void u8(std::uint8_t value) { appendU8(data_, value); }
    void u32(std::uint32_t value) { appendU32(data_, value); }
    void u64(std::uint64_t value) { appendU64(data_, value); }

    void floating(double value) {
        u64(std::bit_cast<std::uint64_t>(value));
    }

    void string(const std::string& value) {
        if (value.size() > maximumStringBytes) {
            throw std::length_error("Agent protocol string exceeds 1 MiB");
        }
        u32(static_cast<std::uint32_t>(value.size()));
        data_.insert(data_.end(), value.begin(), value.end());
    }

    void bytes(std::span<const std::uint8_t> value) {
        if (value.size() > maximumPayloadBytes) {
            throw std::length_error("Agent protocol byte array exceeds 16 MiB");
        }
        u32(static_cast<std::uint32_t>(value.size()));
        data_.insert(data_.end(), value.begin(), value.end());
    }

    void count(std::size_t value) {
        if (value > maximumCollectionItems) {
            throw std::length_error("Agent protocol collection exceeds item limit");
        }
        u32(static_cast<std::uint32_t>(value));
    }

    [[nodiscard]] std::vector<std::uint8_t> take() { return std::move(data_); }

private:
    std::vector<std::uint8_t> data_;
};

class PayloadReader {
public:
    PayloadReader(std::span<const std::uint8_t> data, std::string* error)
        : data_(data), error_(error) {}

    bool u8(std::uint8_t& value) {
        if (!require(1, "uint8")) return false;
        value = data_[position_++];
        return true;
    }

    bool u32(std::uint32_t& value) {
        if (!require(4, "uint32")) return false;
        value = readU32(data_, position_);
        position_ += 4;
        return true;
    }

    bool u64(std::uint64_t& value) {
        if (!require(8, "uint64")) return false;
        value = readU64(data_, position_);
        position_ += 8;
        return true;
    }

    bool floating(double& value) {
        std::uint64_t bits = 0;
        if (!u64(bits)) return false;
        value = std::bit_cast<double>(bits);
        return true;
    }

    bool string(std::string& value) {
        std::uint32_t length = 0;
        if (!u32(length)) return false;
        if (length > maximumStringBytes) {
            fail("string exceeds 1 MiB limit");
            return false;
        }
        if (!require(length, "string bytes")) return false;
        value.assign(
            reinterpret_cast<const char*>(data_.data() + position_), length);
        position_ += length;
        return true;
    }

    bool bytes(std::vector<std::uint8_t>& value) {
        std::uint32_t length = 0;
        if (!u32(length)) return false;
        if (length > maximumPayloadBytes) {
            fail("byte array exceeds 16 MiB limit");
            return false;
        }
        if (!require(length, "byte array")) return false;
        value.assign(data_.begin() + static_cast<std::ptrdiff_t>(position_),
                     data_.begin() + static_cast<std::ptrdiff_t>(position_ + length));
        position_ += length;
        return true;
    }

    bool count(std::uint32_t& value) {
        if (!u32(value)) return false;
        if (value > maximumCollectionItems) {
            fail("collection exceeds item limit");
            return false;
        }
        return true;
    }

    bool finish() {
        if (position_ != data_.size()) {
            fail("payload has trailing bytes");
            return false;
        }
        return true;
    }

    void fail(std::string message) {
        failed_ = true;
        if (error_ && error_->empty()) {
            *error_ = std::move(message);
        }
    }

    [[nodiscard]] bool failed() const noexcept { return failed_; }

private:
    bool require(std::size_t count, const char* description) {
        if (count > data_.size() - position_) {
            fail(std::string("truncated ") + description);
            return false;
        }
        return true;
    }

    std::span<const std::uint8_t> data_;
    std::size_t position_{};
    std::string* error_{};
    bool failed_{};
};

bool validReliability(std::uint8_t value) {
    return value <= static_cast<std::uint8_t>(Reliability::Reliable);
}

bool validDurability(std::uint8_t value) {
    return value <= static_cast<std::uint8_t>(Durability::TransientLocal);
}

void requireNonEmpty(const std::string& value, const char* field) {
    if (value.empty()) {
        throw std::invalid_argument(std::string("Agent protocol ") + field + " is empty");
    }
}

void addIssue(
    DecodeResult& result,
    StreamStatistics& statistics,
    DecodeIssueKind kind,
    std::size_t discardedBytes,
    std::string message) {
    result.issues.push_back({kind, discardedBytes, std::move(message)});
    statistics.discardedBytes += discardedBytes;
}

}  // namespace

DecodeResult StreamDecoder::consume(std::span<const std::uint8_t> bytes) {
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    DecodeResult result;

    while (true) {
        if (buffer_.size() < magic.size()) {
            break;
        }

        const auto magicPosition = std::search(
            buffer_.begin(), buffer_.end(), magic.begin(), magic.end());
        if (magicPosition == buffer_.end()) {
            const auto retained = magicPrefixSuffixLength(buffer_);
            const auto discarded = buffer_.size() - retained;
            if (discarded > 0) {
                addIssue(result, statistics_, DecodeIssueKind::Garbage, discarded,
                         "Discarded bytes before Agent frame magic");
                buffer_.erase(buffer_.begin(),
                              buffer_.end() - static_cast<std::ptrdiff_t>(retained));
            }
            break;
        }

        const auto garbage = static_cast<std::size_t>(
            std::distance(buffer_.begin(), magicPosition));
        if (garbage > 0) {
            addIssue(result, statistics_, DecodeIssueKind::Garbage, garbage,
                     "Discarded bytes before Agent frame magic");
            buffer_.erase(buffer_.begin(), magicPosition);
            continue;
        }

        if (buffer_.size() < frameHeaderSize) {
            break;
        }

        const auto header = std::span<const std::uint8_t>(buffer_).first(frameHeaderSize);
        if (header[4] != protocolVersion) {
            ++statistics_.unsupportedVersions;
            addIssue(result, statistics_, DecodeIssueKind::UnsupportedVersion, 1,
                     "Unsupported Agent protocol version " + std::to_string(header[4]));
            buffer_.erase(buffer_.begin());
            continue;
        }
        if (!isKnownMessageType(header[5])) {
            ++statistics_.unknownMessageTypes;
            addIssue(result, statistics_, DecodeIssueKind::UnknownMessageType, 1,
                     "Unknown Agent message type " + std::to_string(header[5]));
            buffer_.erase(buffer_.begin());
            continue;
        }

        const auto payloadBytes = readU32(header, 8);
        if (payloadBytes > maximumPayloadBytes) {
            ++statistics_.oversizedPayloads;
            addIssue(result, statistics_, DecodeIssueKind::OversizedPayload, 1,
                     "Agent frame payload exceeds 16 MiB limit");
            buffer_.erase(buffer_.begin());
            continue;
        }
        const auto totalBytes = frameHeaderSize + static_cast<std::size_t>(payloadBytes);
        if (buffer_.size() < totalBytes) {
            break;
        }

        const auto payload = std::span<const std::uint8_t>(buffer_)
                                 .subspan(frameHeaderSize, payloadBytes);
        const auto storedCrc = readU32(header, 36);
        const auto calculatedCrc = frameCrc(header.first(36), payload);
        if (storedCrc != calculatedCrc) {
            ++statistics_.checksumErrors;
            addIssue(result, statistics_, DecodeIssueKind::ChecksumMismatch, 1,
                     "Agent frame CRC32 mismatch");
            buffer_.erase(buffer_.begin());
            continue;
        }

        Frame frame;
        frame.type = static_cast<MessageType>(header[5]);
        frame.flags = readU16(header, 6);
        frame.sequence = readU64(header, 12);
        frame.sourceTimestamp = std::bit_cast<std::int64_t>(readU64(header, 20));
        frame.agentReceiveTimestamp = std::bit_cast<std::int64_t>(readU64(header, 28));
        frame.payload.assign(payload.begin(), payload.end());
        result.frames.push_back(std::move(frame));
        ++statistics_.decodedFrames;
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(totalBytes));
    }

    return result;
}

void StreamDecoder::reset() {
    buffer_.clear();
    statistics_ = {};
}

std::size_t StreamDecoder::bufferedBytes() const noexcept {
    return buffer_.size();
}

const StreamStatistics& StreamDecoder::statistics() const noexcept {
    return statistics_;
}

bool isKnownMessageType(std::uint8_t value) noexcept {
    return value >= static_cast<std::uint8_t>(MessageType::Hello) &&
           value <= static_cast<std::uint8_t>(MessageType::Pong);
}

std::string toString(MessageType type) {
    switch (type) {
    case MessageType::Hello: return "hello";
    case MessageType::HelloAck: return "hello_ack";
    case MessageType::TopicCatalog: return "topic_catalog";
    case MessageType::Subscribe: return "subscribe";
    case MessageType::Unsubscribe: return "unsubscribe";
    case MessageType::SampleBatch: return "sample_batch";
    case MessageType::Error: return "error";
    case MessageType::Ping: return "ping";
    case MessageType::Pong: return "pong";
    }
    return "unknown";
}

std::uint32_t crc32Ieee(std::span<const std::uint8_t> bytes) noexcept {
    return ~updateCrc32(0xFFFFFFFFU, bytes);
}

std::vector<std::uint8_t> encodeFrame(const Frame& frame) {
    if (!isKnownMessageType(static_cast<std::uint8_t>(frame.type))) {
        throw std::invalid_argument("Unknown Agent message type");
    }
    if (frame.payload.size() > maximumPayloadBytes) {
        throw std::length_error("Agent frame payload exceeds 16 MiB limit");
    }

    std::vector<std::uint8_t> output;
    output.reserve(frameHeaderSize + frame.payload.size());
    output.insert(output.end(), magic.begin(), magic.end());
    appendU8(output, protocolVersion);
    appendU8(output, static_cast<std::uint8_t>(frame.type));
    appendU16(output, frame.flags);
    appendU32(output, static_cast<std::uint32_t>(frame.payload.size()));
    appendU64(output, frame.sequence);
    appendU64(output, static_cast<std::uint64_t>(frame.sourceTimestamp));
    appendU64(output, static_cast<std::uint64_t>(frame.agentReceiveTimestamp));
    appendU32(output, 0);
    output.insert(output.end(), frame.payload.begin(), frame.payload.end());

    const auto crc = frameCrc(
        std::span<const std::uint8_t>(output).first(36),
        std::span<const std::uint8_t>(output).subspan(frameHeaderSize));
    for (unsigned int index = 0; index < 4U; ++index) {
        output[36 + index] = static_cast<std::uint8_t>((crc >> (index * 8U)) & 0xFFU);
    }
    return output;
}

std::vector<std::uint8_t> encodeHello(const Hello& value) {
    requireNonEmpty(value.agentId, "agent id");
    requireNonEmpty(value.softwareVersion, "software version");
    PayloadWriter writer;
    writer.string(value.agentId);
    writer.string(value.softwareVersion);
    writer.string(value.hostName);
    writer.u32(value.capabilities);
    return writer.take();
}

std::optional<Hello> decodeHello(
    std::span<const std::uint8_t> payload,
    std::string* error) {
    if (error) error->clear();
    PayloadReader reader(payload, error);
    Hello value;
    if (!reader.string(value.agentId) || !reader.string(value.softwareVersion) ||
        !reader.string(value.hostName) || !reader.u32(value.capabilities) ||
        !reader.finish()) {
        return std::nullopt;
    }
    if (value.agentId.empty() || value.softwareVersion.empty()) {
        if (error) *error = "hello agent id and software version must not be empty";
        return std::nullopt;
    }
    return value;
}

std::vector<std::uint8_t> encodeHelloAck(const HelloAck& value) {
    requireNonEmpty(value.clientName, "client name");
    requireNonEmpty(value.softwareVersion, "software version");
    PayloadWriter writer;
    writer.string(value.clientName);
    writer.string(value.softwareVersion);
    writer.u32(value.requestedCapabilities);
    return writer.take();
}

std::optional<HelloAck> decodeHelloAck(
    std::span<const std::uint8_t> payload,
    std::string* error) {
    if (error) error->clear();
    PayloadReader reader(payload, error);
    HelloAck value;
    if (!reader.string(value.clientName) || !reader.string(value.softwareVersion) ||
        !reader.u32(value.requestedCapabilities) || !reader.finish()) {
        return std::nullopt;
    }
    if (value.clientName.empty() || value.softwareVersion.empty()) {
        if (error) *error = "hello ack client name and software version must not be empty";
        return std::nullopt;
    }
    return value;
}

std::vector<std::uint8_t> encodeTopicCatalog(const TopicCatalog& value) {
    PayloadWriter writer;
    writer.u64(value.graphRevision);
    writer.count(value.topics.size());
    for (const auto& topic : value.topics) {
        requireNonEmpty(topic.name, "topic name");
        requireNonEmpty(topic.type, "topic type");
        if (!validReliability(static_cast<std::uint8_t>(topic.reliability)) ||
            !validDurability(static_cast<std::uint8_t>(topic.durability))) {
            throw std::invalid_argument("Agent protocol topic has invalid QoS enum");
        }
        writer.string(topic.name);
        writer.string(topic.type);
        writer.u8(static_cast<std::uint8_t>(topic.reliability));
        writer.u8(static_cast<std::uint8_t>(topic.durability));
    }
    return writer.take();
}

std::optional<TopicCatalog> decodeTopicCatalog(
    std::span<const std::uint8_t> payload,
    std::string* error) {
    if (error) error->clear();
    PayloadReader reader(payload, error);
    TopicCatalog value;
    std::uint32_t count = 0;
    if (!reader.u64(value.graphRevision) || !reader.count(count)) {
        return std::nullopt;
    }
    value.topics.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        TopicDescriptor topic;
        std::uint8_t reliability = 0;
        std::uint8_t durability = 0;
        if (!reader.string(topic.name) || !reader.string(topic.type) ||
            !reader.u8(reliability) || !reader.u8(durability)) {
            return std::nullopt;
        }
        if (!validReliability(reliability) || !validDurability(durability)) {
            reader.fail("topic catalog contains invalid QoS enum");
            return std::nullopt;
        }
        topic.reliability = static_cast<Reliability>(reliability);
        topic.durability = static_cast<Durability>(durability);
        if (topic.name.empty() || topic.type.empty()) {
            reader.fail("topic catalog contains empty topic name or type");
            return std::nullopt;
        }
        value.topics.push_back(std::move(topic));
    }
    if (!reader.finish()) return std::nullopt;
    return value;
}

std::vector<std::uint8_t> encodeSubscriptionRequest(
    const SubscriptionRequest& value) {
    requireNonEmpty(value.topic, "subscription topic");
    requireNonEmpty(value.type, "subscription type");
    if (!validReliability(static_cast<std::uint8_t>(value.reliability))) {
        throw std::invalid_argument("Agent protocol subscription has invalid reliability enum");
    }
    if (value.queueDepth == 0 || value.queueDepth > 1'000'000U) {
        throw std::invalid_argument("Agent protocol queue depth is outside 1..1000000");
    }
    PayloadWriter writer;
    writer.u64(value.requestId);
    writer.string(value.topic);
    writer.string(value.type);
    writer.u8(static_cast<std::uint8_t>(value.reliability));
    writer.u32(value.queueDepth);
    return writer.take();
}

std::optional<SubscriptionRequest> decodeSubscriptionRequest(
    std::span<const std::uint8_t> payload,
    std::string* error) {
    if (error) error->clear();
    PayloadReader reader(payload, error);
    SubscriptionRequest value;
    std::uint8_t reliability = 0;
    if (!reader.u64(value.requestId) || !reader.string(value.topic) ||
        !reader.string(value.type) || !reader.u8(reliability) ||
        !reader.u32(value.queueDepth) || !reader.finish()) {
        return std::nullopt;
    }
    if (!validReliability(reliability)) {
        if (error) *error = "subscription contains invalid reliability enum";
        return std::nullopt;
    }
    if (value.topic.empty() || value.type.empty()) {
        if (error) *error = "subscription topic and type must not be empty";
        return std::nullopt;
    }
    if (value.queueDepth == 0 || value.queueDepth > 1'000'000U) {
        if (error) *error = "subscription queue depth is outside 1..1000000";
        return std::nullopt;
    }
    value.reliability = static_cast<Reliability>(reliability);
    return value;
}

std::vector<std::uint8_t> encodeSampleBatch(const SampleBatch& value) {
    requireNonEmpty(value.topic, "sample topic");
    requireNonEmpty(value.type, "sample type");
    PayloadWriter writer;
    writer.string(value.topic);
    writer.string(value.type);
    writer.bytes(value.serializedData);
    writer.count(value.fields.size());
    for (const auto& field : value.fields) {
        requireNonEmpty(field.path, "sample field path");
        writer.string(field.path);
        writer.string(field.unit);
        if (const auto* numeric = std::get_if<double>(&field.value)) {
            writer.u8(1);
            writer.floating(*numeric);
        } else if (const auto* boolean = std::get_if<bool>(&field.value)) {
            writer.u8(2);
            writer.u8(*boolean ? 1U : 0U);
        } else {
            writer.u8(3);
            writer.string(std::get<std::string>(field.value));
        }
    }
    return writer.take();
}

std::optional<SampleBatch> decodeSampleBatch(
    std::span<const std::uint8_t> payload,
    std::string* error) {
    if (error) error->clear();
    PayloadReader reader(payload, error);
    SampleBatch value;
    std::uint32_t count = 0;
    if (!reader.string(value.topic) || !reader.string(value.type) ||
        !reader.bytes(value.serializedData) || !reader.count(count)) {
        return std::nullopt;
    }
    value.fields.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        FieldValue field;
        std::uint8_t kind = 0;
        if (!reader.string(field.path) || !reader.string(field.unit) || !reader.u8(kind)) {
            return std::nullopt;
        }
        if (kind == 1) {
            double number = 0.0;
            if (!reader.floating(number)) return std::nullopt;
            field.value = number;
        } else if (kind == 2) {
            std::uint8_t boolean = 0;
            if (!reader.u8(boolean)) return std::nullopt;
            if (boolean > 1U) {
                reader.fail("boolean field is not 0 or 1");
                return std::nullopt;
            }
            field.value = boolean != 0U;
        } else if (kind == 3) {
            std::string text;
            if (!reader.string(text)) return std::nullopt;
            field.value = std::move(text);
        } else {
            reader.fail("sample contains unknown field kind");
            return std::nullopt;
        }
        if (field.path.empty()) {
            reader.fail("sample contains empty field path");
            return std::nullopt;
        }
        value.fields.push_back(std::move(field));
    }
    if (!reader.finish()) return std::nullopt;
    if (value.topic.empty() || value.type.empty()) {
        if (error) *error = "sample topic and type must not be empty";
        return std::nullopt;
    }
    return value;
}

std::vector<std::uint8_t> encodeAgentError(const AgentError& value) {
    PayloadWriter writer;
    writer.u32(value.code);
    writer.string(value.context);
    writer.string(value.message);
    return writer.take();
}

std::optional<AgentError> decodeAgentError(
    std::span<const std::uint8_t> payload,
    std::string* error) {
    if (error) error->clear();
    PayloadReader reader(payload, error);
    AgentError value;
    if (!reader.u32(value.code) || !reader.string(value.context) ||
        !reader.string(value.message) || !reader.finish()) {
        return std::nullopt;
    }
    return value;
}

std::vector<std::uint8_t> encodeNonce(std::uint64_t nonce) {
    PayloadWriter writer;
    writer.u64(nonce);
    return writer.take();
}

std::optional<std::uint64_t> decodeNonce(
    std::span<const std::uint8_t> payload,
    std::string* error) {
    if (error) error->clear();
    PayloadReader reader(payload, error);
    std::uint64_t nonce = 0;
    if (!reader.u64(nonce) || !reader.finish()) {
        return std::nullopt;
    }
    return nonce;
}

}  // namespace lab::core::agent

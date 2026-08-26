#include "lab/core/agent_protocol.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace lab::core::agent;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error("agent protocol: " + message);
    }
}

Frame makeFrame(MessageType type, std::uint64_t sequence, std::vector<std::uint8_t> payload) {
    return {type, 0x1234U, sequence, -123'456'789, 987'654'321, std::move(payload)};
}

void append(std::vector<std::uint8_t>& target, const std::vector<std::uint8_t>& source) {
    target.insert(target.end(), source.begin(), source.end());
}

void testCrc32KnownVector() {
    const std::string text = "123456789";
    const auto bytes = std::span(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    require(crc32Ieee(bytes) == 0xCBF43926U, "CRC32/IEEE known vector");
}

void testFrameRoundTripAndFragmentation() {
    const Hello hello{
        "robot-a", "0.1.0", "minipc", capabilityMask(Capability::TopicDiscovery) |
                                            capabilityMask(Capability::SerializedMessages)};
    const auto frame = makeFrame(MessageType::Hello, 42, encodeHello(hello));
    const auto encoded = encodeFrame(frame);
    require(encoded.size() == frameHeaderSize + frame.payload.size(),
            "encoded frame has fixed header and payload");

    StreamDecoder decoder;
    DecodeResult aggregate;
    for (const auto byte : encoded) {
        auto result = decoder.consume(std::span(&byte, 1));
        aggregate.frames.insert(aggregate.frames.end(),
                                result.frames.begin(), result.frames.end());
        aggregate.issues.insert(aggregate.issues.end(),
                                result.issues.begin(), result.issues.end());
    }
    require(aggregate.issues.empty(), "byte-wise fragmentation has no issues");
    require(aggregate.frames.size() == 1 && aggregate.frames.front() == frame,
            "fragmented frame round-trips exactly");
    require(decoder.bufferedBytes() == 0, "decoder consumes complete frame");
    require(decoder.statistics().decodedFrames == 1, "decoded frame statistic");
}

void testStickyFramesAndGarbageRecovery() {
    const auto first = makeFrame(MessageType::Ping, 10, encodeNonce(111));
    const auto second = makeFrame(MessageType::Pong, 11, encodeNonce(222));
    std::vector<std::uint8_t> stream{0x00, 0xFF, 'L', 'D'};
    append(stream, encodeFrame(first));
    append(stream, encodeFrame(second));

    StreamDecoder decoder;
    const auto result = decoder.consume(stream);
    require(result.frames.size() == 2, "sticky frames both decode");
    require(result.frames[0] == first && result.frames[1] == second,
            "sticky frame order preserved");
    require(!result.issues.empty() &&
                result.issues.front().kind == DecodeIssueKind::Garbage,
            "leading garbage is reported");
    require(decoder.statistics().discardedBytes == 4, "all leading garbage counted");
}

void testChecksumAndHeaderRecovery() {
    const auto good = encodeFrame(makeFrame(MessageType::Ping, 99, encodeNonce(99)));

    auto badCrc = encodeFrame(makeFrame(MessageType::Pong, 1, encodeNonce(1)));
    badCrc.back() ^= 0x80U;
    append(badCrc, good);
    StreamDecoder crcDecoder;
    const auto crcResult = crcDecoder.consume(badCrc);
    require(crcResult.frames.size() == 1 && crcResult.frames.front().sequence == 99,
            "decoder resynchronizes after checksum error");
    require(crcDecoder.statistics().checksumErrors == 1,
            "checksum error statistic increments");

    auto badVersion = good;
    badVersion[4] = protocolVersion + 1U;
    append(badVersion, good);
    StreamDecoder versionDecoder;
    const auto versionResult = versionDecoder.consume(badVersion);
    require(versionResult.frames.size() == 1,
            "decoder resynchronizes after unsupported version");
    require(versionDecoder.statistics().unsupportedVersions == 1,
            "unsupported version statistic increments");

    auto unknownType = good;
    unknownType[5] = 0xFEU;
    append(unknownType, good);
    StreamDecoder typeDecoder;
    const auto typeResult = typeDecoder.consume(unknownType);
    require(typeResult.frames.size() == 1,
            "decoder resynchronizes after unknown message type");
    require(typeDecoder.statistics().unknownMessageTypes == 1,
            "unknown type statistic increments");

    auto oversized = good;
    const auto length = maximumPayloadBytes + 1U;
    for (unsigned int index = 0; index < 4U; ++index) {
        oversized[8 + index] = static_cast<std::uint8_t>((length >> (index * 8U)) & 0xFFU);
    }
    append(oversized, good);
    StreamDecoder sizeDecoder;
    const auto sizeResult = sizeDecoder.consume(oversized);
    require(sizeResult.frames.size() == 1,
            "decoder resynchronizes after oversized payload header");
    require(sizeDecoder.statistics().oversizedPayloads == 1,
            "oversized payload statistic increments");
}

void testHandshakePayloads() {
    const Hello hello{
        "robot-main", "0.4.0", "robot-minipc",
        capabilityMask(Capability::TopicDiscovery) |
            capabilityMask(Capability::SerializedMessages) |
            capabilityMask(Capability::NumericFields)};
    std::string error;
    const auto decodedHello = decodeHello(encodeHello(hello), &error);
    require(decodedHello && *decodedHello == hello && error.empty(),
            "hello payload round-trip");

    const HelloAck ack{
        "Lab Debugger", "0.5.0",
        capabilityMask(Capability::TopicDiscovery) |
            capabilityMask(Capability::NumericFields)};
    const auto decodedAck = decodeHelloAck(encodeHelloAck(ack), &error);
    require(decodedAck && *decodedAck == ack, "hello ack payload round-trip");
}

void testTopicAndSubscriptionPayloads() {
    const TopicCatalog catalog{
        77,
        {{"/cmd_vel", "geometry_msgs/msg/Twist", Reliability::Reliable,
          Durability::Volatile},
         {"/imu/data", "sensor_msgs/msg/Imu", Reliability::BestEffort,
          Durability::Volatile}}};
    std::string error;
    const auto decodedCatalog = decodeTopicCatalog(encodeTopicCatalog(catalog), &error);
    require(decodedCatalog && *decodedCatalog == catalog,
            "topic catalog payload round-trip");

    const SubscriptionRequest request{
        1234, "/imu/data", "sensor_msgs/msg/Imu", Reliability::BestEffort, 50};
    const auto decodedRequest = decodeSubscriptionRequest(
        encodeSubscriptionRequest(request), &error);
    require(decodedRequest && *decodedRequest == request,
            "subscription payload round-trip");
}

void testSampleAndErrorPayloads() {
    const SampleBatch sample{
        "/odom",
        "nav_msgs/msg/Odometry",
        {0x00, 0x01, 0xCD, 0xAB},
        {{"pose.pose.position.x", "m", 1.25},
         {"status.valid", "", true},
         {"frame_id", "", std::string("odom")}}};
    std::string error;
    const auto decodedSample = decodeSampleBatch(encodeSampleBatch(sample), &error);
    require(decodedSample && *decodedSample == sample,
            "sample batch preserves raw CDR and typed fields");

    const AgentError agentError{404, "/missing", "Topic no longer exists"};
    const auto decodedError = decodeAgentError(encodeAgentError(agentError), &error);
    require(decodedError && *decodedError == agentError,
            "agent error payload round-trip");

    constexpr std::uint64_t nonce = 0x1020304050607080ULL;
    const auto decodedNonce = decodeNonce(encodeNonce(nonce), &error);
    require(decodedNonce && *decodedNonce == nonce, "heartbeat nonce round-trip");
}

void testMalformedPayloadsAreRejected() {
    std::string error;
    auto hello = encodeHello({"a", "b", "c", 1});
    hello.pop_back();
    require(!decodeHello(hello, &error) && !error.empty(),
            "truncated hello is rejected with context");

    auto nonce = encodeNonce(10);
    nonce.push_back(0);
    require(!decodeNonce(nonce, &error) && error.find("trailing") != std::string::npos,
            "trailing payload bytes are rejected");

    auto booleanSample = encodeSampleBatch(
        {"/state", "std_msgs/msg/Bool", {}, {{"data", "", true}}});
    booleanSample.back() = 2;
    require(!decodeSampleBatch(booleanSample, &error) &&
                error.find("boolean") != std::string::npos,
            "non-canonical boolean is rejected");

    auto invalidDepth = encodeSubscriptionRequest(
        {1, "/topic", "std_msgs/msg/Float64", Reliability::Reliable, 10});
    std::fill(invalidDepth.end() - 4, invalidDepth.end(), 0);
    require(!decodeSubscriptionRequest(invalidDepth, &error) &&
                error.find("queue depth") != std::string::npos,
            "zero subscription queue depth is rejected");

    const std::array<std::uint8_t, 4> oversizedStringLength{0x01, 0x00, 0x10, 0x00};
    require(!decodeHello(oversizedStringLength, &error) &&
                error.find("1 MiB") != std::string::npos,
            "oversized string declaration is rejected before allocation");
}

}  // namespace

int main() {
    try {
        testCrc32KnownVector();
        testFrameRoundTripAndFragmentation();
        testStickyFramesAndGarbageRecovery();
        testChecksumAndHeaderRecovery();
        testHandshakePayloads();
        testTopicAndSubscriptionPayloads();
        testSampleAndErrorPayloads();
        testMalformedPayloadsAreRejected();
        std::cout << "All Remote Agent protocol tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Test failed: " << exception.what() << '\n';
        return 1;
    }
}

#include "lab/core/remote_agent_session.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace lab::core::agent;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("agent server session: " + message);
}

Frame clientFrame(
    MessageType type,
    std::uint64_t sequence,
    std::vector<std::uint8_t> payload) {
    return {type, 0, sequence, 10, 20, std::move(payload)};
}

std::vector<Frame> decodeAll(const std::vector<std::vector<std::uint8_t>>& encoded) {
    StreamDecoder decoder;
    std::vector<Frame> frames;
    for (const auto& bytes : encoded) {
        auto result = decoder.consume(bytes);
        require(result.issues.empty(), "server output must decode without issues");
        frames.insert(frames.end(), result.frames.begin(), result.frames.end());
    }
    return frames;
}

Hello agentHello() {
    return {"robot-agent",
            "0.1.0",
            "ubuntu-host",
            capabilityMask(Capability::TopicDiscovery) |
                capabilityMask(Capability::SerializedMessages) |
                capabilityMask(Capability::NumericFields) |
                capabilityMask(Capability::TextFields)};
}

void finishHandshakeWithCapabilities(
    ServerSession& session,
    std::uint32_t capabilities,
    std::uint64_t sequence = 0) {
    const HelloAck ack{
        "Lab Debugger",
        "0.5.0",
        capabilities};
    const auto result = session.consume(
        encodeFrame(clientFrame(MessageType::HelloAck, sequence, encodeHelloAck(ack))));
    require(!result.fatalError && session.state() == ServerSessionState::Ready,
            "valid hello ack completes server handshake");
    require(session.clientIdentity() == ack,
            "server retains client identity");
}

void finishHandshake(ServerSession& session, std::uint64_t sequence = 0) {
    finishHandshakeWithCapabilities(
        session,
        capabilityMask(Capability::TopicDiscovery) |
            capabilityMask(Capability::SerializedMessages) |
            capabilityMask(Capability::NumericFields),
        sequence);
}

void testStartAndFragmentedHandshake() {
    ServerSession session(agentHello());
    const auto helloBytes = session.start();
    StreamDecoder decoder;
    const auto decoded = decoder.consume(helloBytes);
    require(decoded.frames.size() == 1 && decoded.frames[0].type == MessageType::Hello,
            "start emits hello");
    std::string error;
    require(decodeHello(decoded.frames[0].payload, &error) == agentHello(),
            "hello identity round-trips");

    const HelloAck ack{
        "fragmented-client", "0.5.0", capabilityMask(Capability::TopicDiscovery)};
    const auto ackBytes = encodeFrame(
        clientFrame(MessageType::HelloAck, 10, encodeHelloAck(ack)));
    const auto first = session.consume(std::span(ackBytes).first(7));
    require(!first.fatalError && session.state() == ServerSessionState::AwaitingHelloAck,
            "partial hello ack is buffered");
    const auto second = session.consume(std::span(ackBytes).subspan(7));
    require(!second.fatalError && session.state() == ServerSessionState::Ready,
            "remaining hello ack completes handshake");
}

void testActionsHeartbeatAndServerMessages() {
    ServerSession session(agentHello());
    const auto helloBytes = session.start();
    static_cast<void>(helloBytes);
    finishHandshake(session);

    const SubscriptionRequest request{
        77, "/imu", "sensor_msgs/msg/Imu", Reliability::BestEffort, 50};
    std::vector<std::uint8_t> stream;
    const auto appendFrame = [&](const Frame& frame) {
        const auto bytes = encodeFrame(frame);
        stream.insert(stream.end(), bytes.begin(), bytes.end());
    };
    appendFrame(clientFrame(MessageType::TopicCatalogRequest, 1, {}));
    appendFrame(clientFrame(MessageType::Subscribe, 2, encodeSubscriptionRequest(request)));
    appendFrame(clientFrame(MessageType::Unsubscribe, 3, encodeSubscriptionRequest(request)));
    appendFrame(clientFrame(MessageType::Ping, 4, encodeNonce(0x1234)));

    const auto result = session.consume(stream);
    require(!result.fatalError && result.actions.size() == 3,
            "sticky client controls become three host actions");
    require(std::holds_alternative<CatalogRequestAction>(result.actions[0]),
            "catalog action order");
    require(std::get<SubscribeAction>(result.actions[1]).request == request,
            "subscribe action retains request");
    require(std::get<UnsubscribeAction>(result.actions[2]).request == request,
            "unsubscribe action retains request");
    const auto pong = decodeAll(result.outboundFrames);
    std::string error;
    require(pong.size() == 1 && pong[0].type == MessageType::Pong &&
                decodeNonce(pong[0].payload, &error) == 0x1234,
            "ping creates matching pong");

    const TopicCatalog catalog{
        4,
        {{"/imu", "sensor_msgs/msg/Imu", Reliability::BestEffort,
          Durability::Volatile}}};
    const auto catalogBytes = session.makeTopicCatalog(catalog);
    const SampleBatch sample{
        "/imu", "sensor_msgs/msg/Imu", {1, 2, 3}, {{"orientation.w", "", 1.0}}};
    const auto sampleBytes = session.makeSample(sample, 100, 200);
    const auto pingBytes = session.makePing(99);
    require(catalogBytes && sampleBytes && pingBytes,
            "ready server can emit catalog, sample and ping");
    const auto frames = decodeAll({*catalogBytes, *sampleBytes, *pingBytes});
    require(frames.size() == 3 && frames[0].type == MessageType::TopicCatalog &&
                frames[1].type == MessageType::SampleBatch &&
                frames[2].type == MessageType::Ping,
            "server message order is stable");
    require(frames[0].sequence < frames[1].sequence &&
                frames[1].sequence < frames[2].sequence,
            "all server output shares a strictly increasing sequence");
    require(frames[1].sourceTimestamp == 100 &&
                frames[1].agentReceiveTimestamp == 200,
            "sample preserves both Agent timestamps");
}

void testProtocolFailuresAndRecoveryIssues() {
    ServerSession unsupported(agentHello());
    static_cast<void>(unsupported.start());
    const HelloAck badAck{"bad", "1", 0x80000000U};
    const auto badResult = unsupported.consume(encodeFrame(
        clientFrame(MessageType::HelloAck, 0, encodeHelloAck(badAck))));
    require(badResult.fatalError && unsupported.state() == ServerSessionState::Error,
            "unsupported capability request is fatal");
    const auto errorFrames = decodeAll(badResult.outboundFrames);
    require(errorFrames.size() == 1 && errorFrames[0].type == MessageType::Error,
            "fatal protocol failure sends structured error");

    ServerSession duplicate(agentHello());
    static_cast<void>(duplicate.start());
    finishHandshake(duplicate, 5);
    const auto catalogRequest = encodeFrame(
        clientFrame(MessageType::TopicCatalogRequest, 5, {}));
    const auto duplicateResult = duplicate.consume(catalogRequest);
    require(duplicateResult.fatalError &&
                duplicateResult.fatalError->find("strictly increasing") != std::string::npos,
            "duplicate client sequence is fatal");

    ServerSession recovery(agentHello());
    static_cast<void>(recovery.start());
    finishHandshake(recovery);
    auto corrupt = encodeFrame(clientFrame(MessageType::Ping, 1, encodeNonce(1)));
    corrupt.back() ^= 0x40U;
    const auto valid = encodeFrame(
        clientFrame(MessageType::TopicCatalogRequest, 1, {}));
    corrupt.insert(corrupt.end(), valid.begin(), valid.end());
    const auto recoveryResult = recovery.consume(corrupt);
    require(!recoveryResult.fatalError && !recoveryResult.decodeIssues.empty() &&
                recoveryResult.actions.size() == 1,
            "CRC damage reports issues and resynchronizes to next valid action");
}

void testOutputRequiresReadyState() {
    ServerSession session(agentHello());
    require(!session.makePing(1) && !session.makeTopicCatalog({}),
            "idle session rejects output");
    static_cast<void>(session.start());
    require(!session.makeSample({"/x", "std_msgs/msg/Float64", {}, {}}, 1, 2),
            "awaiting session rejects sample output");
}

void testNegotiatedCapabilitiesAreEnforced() {
    const SampleBatch sample{
        "/mixed",
        "custom_msgs/msg/Mixed",
        {1, 2, 3, 4},
        {{"number", "", 2.5},
         {"valid", "", true},
         {"label", "", std::string("ready")}}};
    std::string error;

    ServerSession serializedOnly(agentHello());
    static_cast<void>(serializedOnly.start());
    finishHandshakeWithCapabilities(
        serializedOnly, capabilityMask(Capability::SerializedMessages));
    require(!serializedOnly.makeTopicCatalog({1, {}}),
            "topic catalog is not sent without topic discovery negotiation");
    const auto rawFrame = serializedOnly.makeSample(sample, 0, 456);
    require(rawFrame.has_value(), "serialized-only client receives raw samples");
    const auto rawFrames = decodeAll({*rawFrame});
    const auto rawSample = decodeSampleBatch(rawFrames[0].payload, &error);
    require(rawSample && rawSample->serializedData == sample.serializedData &&
                rawSample->fields.empty(),
            "serialized-only sample strips all structured fields");
    require(rawFrames[0].sourceTimestamp == 456 &&
                rawFrames[0].agentReceiveTimestamp == 456,
            "zero source timestamp falls back to Agent receive time");

    ServerSession numericOnly(agentHello());
    static_cast<void>(numericOnly.start());
    finishHandshakeWithCapabilities(
        numericOnly, capabilityMask(Capability::NumericFields));
    const auto numericFrame = numericOnly.makeSample(sample, 100, 200);
    require(numericFrame.has_value(), "numeric-only client receives mapped samples");
    const auto numericFrames = decodeAll({*numericFrame});
    const auto numericSample = decodeSampleBatch(numericFrames[0].payload, &error);
    require(numericSample && numericSample->serializedData.empty() &&
                numericSample->fields.size() == 2 &&
                std::holds_alternative<double>(numericSample->fields[0].value) &&
                std::holds_alternative<bool>(numericSample->fields[1].value),
            "numeric-only sample strips raw CDR and text fields");

    ServerSession discoveryOnly(agentHello());
    static_cast<void>(discoveryOnly.start());
    finishHandshakeWithCapabilities(
        discoveryOnly, capabilityMask(Capability::TopicDiscovery));
    const SubscriptionRequest request{
        9, "/x", "std_msgs/msg/Float64", Reliability::Reliable, 10};
    const auto invalidSubscribe = discoveryOnly.consume(encodeFrame(
        clientFrame(MessageType::Subscribe, 1, encodeSubscriptionRequest(request))));
    require(invalidSubscribe.fatalError &&
                invalidSubscribe.fatalError->find("sample capability") !=
                    std::string::npos,
            "subscribe is rejected when no sample capability was negotiated");

    auto graphIdentity = agentHello();
    graphIdentity.capabilities |= capabilityMask(Capability::GraphUpdates);
    ServerSession invalidGraphDependency(graphIdentity);
    static_cast<void>(invalidGraphDependency.start());
    const HelloAck graphOnly{
        "Lab Debugger", "0.5.0", capabilityMask(Capability::GraphUpdates)};
    const auto invalidGraphAck = invalidGraphDependency.consume(encodeFrame(
        clientFrame(MessageType::HelloAck, 0, encodeHelloAck(graphOnly))));
    require(invalidGraphAck.fatalError &&
                invalidGraphAck.fatalError->find("topic discovery") !=
                    std::string::npos,
            "graph updates cannot be negotiated without topic discovery");
}

}  // namespace

int main() {
    try {
        testStartAndFragmentedHandshake();
        testActionsHeartbeatAndServerMessages();
        testProtocolFailuresAndRecoveryIssues();
        testOutputRequiresReadyState();
        testNegotiatedCapabilitiesAreEnforced();
        std::cout << "All Remote Agent server session tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Test failed: " << exception.what() << '\n';
        return 1;
    }
}

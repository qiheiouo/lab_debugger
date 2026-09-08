#include "lab/adapters/remote_agent/remote_agent_source.hpp"
#include "lab/core/remote_agent_session.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace lab::core;
using namespace lab::core::agent;
using namespace lab::adapters::remote_agent;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error("remote agent: " + message);
    }
}

bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 2500) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

struct Events {
    std::mutex mutex;
    std::vector<DataChunk> chunks;
    std::vector<DataSample> samples;
    std::vector<std::vector<DataSample>> sampleBatches;
    std::vector<SourceState> states;
    std::vector<std::string> errors;
    std::vector<Hello> hellos;
    std::vector<TopicCatalog> catalogs;
    std::vector<TopicFieldCatalog> fieldCatalogs;
    std::vector<SampleBatch> batches;
    std::vector<DecodeIssue> issues;
    std::vector<ClockSyncEstimate> clockEstimates;

    DataSourceCallbacks sourceCallbacks() {
        return {
            [this](const DataChunk& chunk) {
                std::scoped_lock lock(mutex);
                chunks.push_back(chunk);
            },
            [this](SourceState state) {
                std::scoped_lock lock(mutex);
                states.push_back(state);
            },
            [this](const std::string& error) {
                std::scoped_lock lock(mutex);
                errors.push_back(error);
            },
            [this](const DataSample& sample) {
                std::scoped_lock lock(mutex);
                samples.push_back(sample);
            },
            [this](std::span<const DataSample> batch) {
                std::scoped_lock lock(mutex);
                sampleBatches.emplace_back(batch.begin(), batch.end());
            }};
    }

    RemoteAgentCallbacks agentCallbacks() {
        return {
            [this](const Hello& hello) {
                std::scoped_lock lock(mutex);
                hellos.push_back(hello);
            },
            [this](const TopicCatalog& catalog) {
                std::scoped_lock lock(mutex);
                catalogs.push_back(catalog);
            },
            [this](const SampleBatch& batch) {
                std::scoped_lock lock(mutex);
                batches.push_back(batch);
            },
            [this](const DecodeIssue& issue) {
                std::scoped_lock lock(mutex);
                issues.push_back(issue);
            },
            [this](const ClockSyncEstimate& estimate) {
                std::scoped_lock lock(mutex);
                clockEstimates.push_back(estimate);
            },
            [this](const TopicFieldCatalog& catalog) {
                std::scoped_lock lock(mutex);
                fieldCatalogs.push_back(catalog);
            }};
    }

    bool receivedApplicationData() {
        std::scoped_lock lock(mutex);
        return chunks.size() == 1 && samples.size() == 2 &&
               sampleBatches.size() == 1 && sampleBatches.front().size() == 2 &&
               batches.size() == 1;
    }

    bool receivedCatalogAndIssue() {
        std::scoped_lock lock(mutex);
        return catalogs.size() == 1 && fieldCatalogs.size() == 1 && !issues.empty();
    }

    bool hasSequenceError() {
        std::scoped_lock lock(mutex);
        return std::any_of(errors.begin(), errors.end(), [](const std::string& error) {
            return error.find("strictly increasing") != std::string::npos;
        });
    }

    bool hasHandshakeCloseError() {
        std::scoped_lock lock(mutex);
        return std::any_of(errors.begin(), errors.end(), [](const std::string& error) {
            return error.find("before handshake completed") != std::string::npos;
        });
    }

    bool hasClockEstimate() {
        std::scoped_lock lock(mutex);
        return !clockEstimates.empty();
    }
};

Frame serverFrame(MessageType type, std::uint64_t sequence, std::vector<std::uint8_t> payload) {
    return {type, 0, sequence, 1'000'000 + static_cast<Timestamp>(sequence),
            2'000'000 + static_cast<Timestamp>(sequence), std::move(payload)};
}

void send(QTcpSocket& socket, const std::vector<std::uint8_t>& bytes) {
    const QByteArray data(
        reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size()));
    require(socket.write(data) == data.size(), "fixture writes complete frame");
    socket.flush();
}

std::vector<Frame> receiveFrames(
    QTcpSocket& socket,
    StreamDecoder& decoder,
    std::size_t minimum,
    int timeoutMs = 2500) {
    std::vector<Frame> frames;
    require(waitFor([&] {
        const auto bytes = socket.readAll();
        if (!bytes.isEmpty()) {
            const auto first = reinterpret_cast<const std::uint8_t*>(bytes.constData());
            auto result = decoder.consume(
                std::span(first, static_cast<std::size_t>(bytes.size())));
            require(result.issues.empty(), "client control stream decodes cleanly");
            frames.insert(frames.end(), result.frames.begin(), result.frames.end());
        }
        return frames.size() >= minimum;
    }, timeoutMs), "fixture receives expected control frames");
    return frames;
}

void testIdleCloseIsSilent() {
    RemoteAgentSource source;
    Events events;
    source.setCallbacks(events.sourceCallbacks());
    source.close();
    std::scoped_lock lock(events.mutex);
    require(events.states.empty(), "closing an unopened endpoint is idempotent");
}

void testDisconnectBeforeHelloIsError() {
    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost, 0), "early-close fixture listens");
    RemoteAgentSource source;
    Events events;
    source.setCallbacks(events.sourceCallbacks());
    source.setSettings({"127.0.0.1", server.serverPort(), "Agent test client", "0.5.0"});
    require(source.open(), "early-close connection attempt accepted");
    require(waitFor([&] { return server.hasPendingConnections(); }),
            "early-close fixture accepts client");
    auto* peer = server.nextPendingConnection();
    require(waitFor([&] {
                return source.handshakeState() == HandshakeState::AwaitingHello;
            }),
            "early-close client waits for hello");
    peer->disconnectFromHost();
    require(waitFor([&] {
                return source.handshakeState() == HandshakeState::Error &&
                       events.hasHandshakeCloseError();
            }),
            "disconnect before hello is reported as handshake failure");
    source.close();
    peer->deleteLater();
}

void testHandshakeCatalogSamplesAndControls() {
    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost, 0), "fixture listens");

    RemoteAgentSource source;
    Events events;
    source.setCallbacks(events.sourceCallbacks());
    source.setAgentCallbacks(events.agentCallbacks());
    source.setSettings({"127.0.0.1", server.serverPort(), "Agent test client", "0.5.0"});
    require(source.open(), "connection attempt accepted");
    require(waitFor([&] { return server.hasPendingConnections(); }),
            "fixture accepts client");
    auto* peer = server.nextPendingConnection();
    require(peer != nullptr, "accepted peer exists");
    require(waitFor([&] {
                return source.handshakeState() == HandshakeState::AwaitingHello;
            }),
            "client waits for hello before becoming open");
    require(!source.isOpen(), "TCP connection alone does not finish handshake");

    const Hello hello{
        "robot-a", "0.1.0", "robot-host",
        capabilityMask(Capability::TopicDiscovery) |
            capabilityMask(Capability::SerializedMessages) |
            capabilityMask(Capability::NumericFields) |
            capabilityMask(Capability::TextFields) |
            capabilityMask(Capability::TopicFieldCapabilities)};
    const auto helloBytes = encodeFrame(serverFrame(MessageType::Hello, 10, encodeHello(hello)));
    send(*peer, std::vector<std::uint8_t>(helloBytes.begin(), helloBytes.begin() + 7));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    require(!source.isOpen(), "fragmented hello remains incomplete");
    send(*peer, std::vector<std::uint8_t>(helloBytes.begin() + 7, helloBytes.end()));
    require(waitFor([&] {
                std::scoped_lock lock(events.mutex);
                return source.isOpen() && source.handshakeState() == HandshakeState::Ready &&
                       events.hellos.size() == 1;
            }),
            "valid hello completes handshake");

    StreamDecoder clientDecoder;
    const auto handshakeFrames = receiveFrames(*peer, clientDecoder, 2);
    require(handshakeFrames[0].type == MessageType::HelloAck,
            "hello acknowledgement is first client frame");
    std::string error;
    const auto ack = decodeHelloAck(handshakeFrames[0].payload, &error);
    require(ack && ack->clientName == "Agent test client" &&
                (ack->requestedCapabilities & capabilityMask(Capability::TextFields)) != 0 &&
                (ack->requestedCapabilities &
                 capabilityMask(Capability::TopicFieldCapabilities)) != 0,
            "hello acknowledgement carries negotiated identity and capabilities");
    require(handshakeFrames[1].type == MessageType::TopicCatalogRequest &&
                handshakeFrames[1].payload.empty(),
            "client automatically requests initial topic catalog");

    const TopicCatalog catalog{
        3,
        {{"/imu", "sensor_msgs/msg/Imu", Reliability::BestEffort,
          Durability::Volatile}}};
    auto corrupt = encodeFrame(serverFrame(MessageType::Ping, 11, encodeNonce(1)));
    corrupt.back() ^= 0x80U;
    auto catalogBytes = encodeFrame(
        serverFrame(MessageType::TopicCatalog, 11, encodeTopicCatalog(catalog)));
    corrupt.insert(corrupt.end(), catalogBytes.begin(), catalogBytes.end());
    const TopicFieldCatalog fieldCatalog{
        3,
        {{"/imu", "sensor_msgs/msg/Imu", FieldMappingKind::BuiltIn,
          "Built-in semantic mapper preserves known field units"}}};
    const auto fieldCatalogBytes = encodeFrame(serverFrame(
        MessageType::TopicFieldCatalog, 12, encodeTopicFieldCatalog(fieldCatalog)));
    corrupt.insert(corrupt.end(), fieldCatalogBytes.begin(), fieldCatalogBytes.end());

    const SampleBatch batch{
        "/imu",
        "sensor_msgs/msg/Imu",
        {0x00, 0x01, 0xCD, 0xAB},
        {{"angular_velocity.x", "rad/s", 1.25},
         {"calibrated", "", true},
         {"frame_id", "", std::string("imu_link")}}};
    auto sampleBytes = encodeFrame(
        serverFrame(MessageType::SampleBatch, 13, encodeSampleBatch(batch)));
    corrupt.insert(corrupt.end(), sampleBytes.begin(), sampleBytes.end());
    send(*peer, corrupt);
    require(waitFor([&] {
                return events.receivedCatalogAndIssue() && events.receivedApplicationData();
            }),
            "decoder recovers and publishes catalog, CDR and numeric fields");

    {
        std::scoped_lock lock(events.mutex);
        require(events.chunks[0].payload == batch.serializedData &&
                    events.chunks[0].sourceId.find("robot-a:/imu") != std::string::npos,
                "raw CDR is preserved with agent/topic identity");
        require(events.samples[0].field == "/imu.angular_velocity.x" &&
                    events.samples[0].value == 1.25 &&
                    events.samples[0].unit == "rad/s",
                "numeric field is published without conversion loss");
        require(events.samples[1].field == "/imu.calibrated" &&
                    events.samples[1].value == 1.0,
                "boolean field is exposed as plottable 0/1");
    }

    const SubscriptionRequest request{
        44, "/imu", "sensor_msgs/msg/Imu", Reliability::BestEffort, 25};
    require(source.requestTopicCatalog(), "manual catalog refresh accepted");
    require(source.subscribe(request), "subscription request accepted");
    require(source.unsubscribe(request), "unsubscribe request accepted");
    send(*peer, encodeFrame(serverFrame(MessageType::Ping, 14, encodeNonce(0xCAFE))));

    const auto controls = receiveFrames(*peer, clientDecoder, 4);
    require(controls[0].type == MessageType::TopicCatalogRequest,
            "manual refresh emits catalog request");
    require(controls[1].type == MessageType::Subscribe &&
                decodeSubscriptionRequest(controls[1].payload, &error) == request,
            "subscribe payload is exact");
    require(controls[2].type == MessageType::Unsubscribe &&
                decodeSubscriptionRequest(controls[2].payload, &error) == request,
            "unsubscribe payload is exact");
    require(controls[3].type == MessageType::Pong &&
                decodeNonce(controls[3].payload, &error) == 0xCAFE,
            "heartbeat nonce is echoed in pong");

    const auto stats = source.statistics();
    require(stats.receivedBytes > 0 && stats.transmittedBytes > 0 &&
                stats.receivedChunks > 0 && stats.transmittedChunks >= 6,
            "wire-level statistics include data and control frames");

    send(*peer, encodeFrame(serverFrame(MessageType::Pong, 14, encodeNonce(2))));
    require(waitFor([&] {
                return source.handshakeState() == HandshakeState::Error &&
                       events.hasSequenceError();
            }),
            "duplicate inbound sequence closes the protocol session");
    source.close();
    peer->deleteLater();
}

void testCapabilityAwareHandshakeWithoutTopicDiscovery() {
    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost, 0), "capability fixture listens");
    RemoteAgentSource source;
    Events events;
    source.setCallbacks(events.sourceCallbacks());
    source.setSettings(
        {"127.0.0.1", server.serverPort(), "Capability client", "0.7.0", false});
    require(source.open(), "capability-aware connection starts");
    require(waitFor([&] { return server.hasPendingConnections(); }),
            "capability fixture accepts client");
    auto* peer = server.nextPendingConnection();

    const Hello hello{
        "sample-only-agent",
        "0.1.0",
        "fixture",
        capabilityMask(Capability::SerializedMessages) |
            capabilityMask(Capability::GraphUpdates)};
    send(*peer, encodeFrame(serverFrame(MessageType::Hello, 0, encodeHello(hello))));
    StreamDecoder decoder;
    const auto frames = receiveFrames(*peer, decoder, 1);
    std::string error;
    const auto ack = decodeHelloAck(frames[0].payload, &error);
    require(ack &&
                (ack->requestedCapabilities &
                 capabilityMask(Capability::SerializedMessages)) != 0U &&
                (ack->requestedCapabilities &
                 capabilityMask(Capability::GraphUpdates)) == 0U,
            "client removes graph updates when topic discovery is unavailable");
    require(waitFor([&] { return source.isOpen(); }),
            "sample-only Agent still completes the handshake");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    require(peer->bytesAvailable() == 0,
            "client does not request a catalog without topic discovery");
    require(!source.requestTopicCatalog(),
            "manual catalog request is rejected when capability was not negotiated");
    send(*peer,
         encodeFrame(serverFrame(
             MessageType::TopicFieldCatalog,
             1,
             encodeTopicFieldCatalog({1, {}}))));
    require(waitFor([&] {
                std::scoped_lock lock(events.mutex);
                return source.handshakeState() == HandshakeState::Error &&
                       std::any_of(
                           events.errors.begin(),
                           events.errors.end(),
                           [](const std::string& message) {
                               return message.find("without negotiation") !=
                                      std::string::npos;
                           });
            }),
            "unnegotiated topic field catalog is a protocol error");
    source.close();
    peer->deleteLater();
}

void testActiveClockSynchronization() {
    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost, 0), "clock fixture listens");
    RemoteAgentSource source;
    Events events;
    source.setCallbacks(events.sourceCallbacks());
    source.setAgentCallbacks(events.agentCallbacks());
    source.setSettings(
        {"127.0.0.1", server.serverPort(), "Clock client", "0.8.0", false});
    require(source.open(), "clock synchronization connection starts");
    require(waitFor([&] { return server.hasPendingConnections(); }),
            "clock fixture accepts client");
    auto* peer = server.nextPendingConnection();

    const Hello hello{"clock-agent", "0.1.0", "fixture", 0};
    send(*peer, encodeFrame(serverFrame(MessageType::Hello, 0, encodeHello(hello))));
    StreamDecoder decoder;
    const auto frames = receiveFrames(*peer, decoder, 2);
    const auto ping = std::find_if(frames.begin(), frames.end(), [](const Frame& frame) {
        return frame.type == MessageType::Ping;
    });
    require(ping != frames.end(), "client actively requests a clock sample after handshake");
    std::string error;
    const auto nonce = decodeNonce(ping->payload, &error);
    require(nonce.has_value(), "clock ping carries a valid nonce");

    constexpr Timestamp simulatedAgentOffset = 5'000'000;
    const auto agentTimestamp = ping->sourceTimestamp + simulatedAgentOffset;
    send(*peer,
         encodeFrame({MessageType::Pong,
                      0,
                      1,
                      agentTimestamp,
                      agentTimestamp,
                      encodeNonce(*nonce)}));
    require(waitFor([&] { return events.hasClockEstimate(); }),
            "matching pong publishes a clock estimate");

    ClockSyncEstimate callbackEstimate;
    {
        std::scoped_lock lock(events.mutex);
        callbackEstimate = events.clockEstimates.back();
    }
    require(
        callbackEstimate.offsetNs ==
            agentTimestamp -
                (ping->sourceTimestamp + callbackEstimate.roundTripNs / 2),
        "clock estimate uses the exact on-wire ping timestamp and local receive time");
    require(callbackEstimate.roundTripNs >= 0 &&
                callbackEstimate.uncertaintyNs == callbackEstimate.roundTripNs / 2 &&
                callbackEstimate.sampleCount == 1,
            "clock callback reports RTT uncertainty and sample count");
    require(source.clockSyncEstimate() == callbackEstimate,
            "source exposes the latest clock estimate safely across threads");

    source.close();
    require(!source.clockSyncEstimate(), "closing the source clears stale clock quality");
    peer->deleteLater();
}

void testAutomaticReconnectRestoresSubscriptions() {
    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost, 0), "reconnect fixture listens");
    RemoteAgentSource source;
    Events events;
    source.setCallbacks(events.sourceCallbacks());
    source.setAgentCallbacks(events.agentCallbacks());
    source.setSettings(
        {"127.0.0.1", server.serverPort(), "Reconnect client", "0.7.0", true});
    require(source.open(), "automatic reconnect connection starts");
    require(waitFor([&] { return server.hasPendingConnections(); }),
            "reconnect fixture accepts initial client");
    auto* firstPeer = server.nextPendingConnection();

    const auto identity = Hello{
        "reconnect-agent",
        "0.1.0",
        "fixture",
        capabilityMask(Capability::TopicDiscovery) |
            capabilityMask(Capability::SerializedMessages) |
            capabilityMask(Capability::NumericFields) |
            capabilityMask(Capability::GraphUpdates)};
    ServerSession firstAgent(identity);
    send(*firstPeer, firstAgent.start());
    std::vector<ServerAction> firstActions;
    const auto consumeFirst = [&] {
        const auto bytes = firstPeer->readAll();
        if (bytes.isEmpty()) return;
        const auto first = reinterpret_cast<const std::uint8_t*>(bytes.constData());
        const auto result = firstAgent.consume(
            std::span(first, static_cast<std::size_t>(bytes.size())));
        require(!result.fatalError, "initial reconnect session accepts client controls");
        for (const auto& outbound : result.outboundFrames) send(*firstPeer, outbound);
        firstActions.insert(firstActions.end(), result.actions.begin(), result.actions.end());
    };
    require(waitFor([&] {
                consumeFirst();
                return firstAgent.state() == ServerSessionState::Ready &&
                       std::any_of(firstActions.begin(), firstActions.end(), [](const auto& action) {
                           return std::holds_alternative<CatalogRequestAction>(action);
                       });
            }),
            "initial session reaches ready and requests a catalog");

    const SubscriptionRequest request{
        81, "/temperature", "std_msgs/msg/Float64", Reliability::Reliable, 20};
    require(source.subscribe(request), "subscription is registered before disconnect");
    require(waitFor([&] {
                consumeFirst();
                return std::any_of(firstActions.begin(), firstActions.end(), [&](const auto& action) {
                    const auto* subscribe = std::get_if<SubscribeAction>(&action);
                    return subscribe && subscribe->request == request;
                });
            }),
            "initial Agent receives subscription");

    firstPeer->disconnectFromHost();
    require(waitFor([&] { return server.hasPendingConnections(); }, 3500),
            "client reconnects after an unexpected transport loss");
    firstPeer->deleteLater();
    auto* secondPeer = server.nextPendingConnection();
    ServerSession secondAgent(identity);
    send(*secondPeer, secondAgent.start());
    std::vector<ServerAction> secondActions;
    const auto consumeSecond = [&] {
        const auto bytes = secondPeer->readAll();
        if (bytes.isEmpty()) return;
        const auto first = reinterpret_cast<const std::uint8_t*>(bytes.constData());
        const auto result = secondAgent.consume(
            std::span(first, static_cast<std::size_t>(bytes.size())));
        require(!result.fatalError, "reconnected session accepts restored controls");
        for (const auto& outbound : result.outboundFrames) send(*secondPeer, outbound);
        secondActions.insert(secondActions.end(), result.actions.begin(), result.actions.end());
    };
    require(waitFor([&] {
                consumeSecond();
                const auto hasCatalog = std::any_of(
                    secondActions.begin(), secondActions.end(), [](const auto& action) {
                        return std::holds_alternative<CatalogRequestAction>(action);
                    });
                const auto hasSubscription = std::any_of(
                    secondActions.begin(), secondActions.end(), [&](const auto& action) {
                        const auto* subscribe = std::get_if<SubscribeAction>(&action);
                        return subscribe && subscribe->request == request;
                    });
                return secondAgent.state() == ServerSessionState::Ready && hasCatalog &&
                       hasSubscription;
            }),
            "reconnected client requests a catalog and restores its subscription");

    source.close();
    secondPeer->deleteLater();
    QElapsedTimer quietPeriod;
    quietPeriod.start();
    while (quietPeriod.elapsed() < 500) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    require(!server.hasPendingConnections(),
            "manual close cancels all pending automatic reconnects");
}

void testProtocolFailureDoesNotAutoReconnect() {
    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost, 0), "fatal protocol fixture listens");
    RemoteAgentSource source;
    Events events;
    source.setCallbacks(events.sourceCallbacks());
    source.setSettings(
        {"127.0.0.1", server.serverPort(), "Protocol client", "0.7.0", true});
    require(source.open(), "protocol failure connection starts");
    require(waitFor([&] { return server.hasPendingConnections(); }),
            "protocol failure fixture accepts client");
    auto* peer = server.nextPendingConnection();
    const Hello hello{
        "protocol-agent",
        "0.1.0",
        "fixture",
        capabilityMask(Capability::TopicDiscovery) |
            capabilityMask(Capability::SerializedMessages)};
    send(*peer, encodeFrame(serverFrame(MessageType::Hello, 0, encodeHello(hello))));
    StreamDecoder decoder;
    static_cast<void>(receiveFrames(*peer, decoder, 2));

    send(*peer, encodeFrame(serverFrame(MessageType::Ping, 1, encodeNonce(1))));
    static_cast<void>(receiveFrames(*peer, decoder, 1));
    send(*peer, encodeFrame(serverFrame(MessageType::Ping, 1, encodeNonce(2))));
    require(waitFor([&] {
                return source.handshakeState() == HandshakeState::Error &&
                       events.hasSequenceError();
            }),
            "strict sequence violation enters a fatal protocol state");
    peer->deleteLater();
    QElapsedTimer quietPeriod;
    quietPeriod.start();
    while (quietPeriod.elapsed() < 600) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    require(!server.hasPendingConnections(),
            "fatal protocol errors do not enter an automatic reconnect loop");
    source.close();
}

void testClientAndServerStateMachinesAreCompatible() {
    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost, 0), "compatibility fixture listens");
    RemoteAgentSource source;
    Events events;
    source.setCallbacks(events.sourceCallbacks());
    source.setAgentCallbacks(events.agentCallbacks());
    source.setSettings({"127.0.0.1", server.serverPort(), "Compatibility client", "0.5.0"});
    require(source.open(), "compatibility client starts");
    require(waitFor([&] { return server.hasPendingConnections(); }),
            "compatibility server accepts client");
    auto* peer = server.nextPendingConnection();

    ServerSession agent({
        "compat-agent",
        "0.1.0",
        "fixture",
        capabilityMask(Capability::TopicDiscovery) |
            capabilityMask(Capability::SerializedMessages) |
            capabilityMask(Capability::NumericFields)});
    send(*peer, agent.start());

    std::vector<ServerAction> actions;
    const auto consumeClient = [&] {
        const auto bytes = peer->readAll();
        if (bytes.isEmpty()) return;
        const auto first = reinterpret_cast<const std::uint8_t*>(bytes.constData());
        const auto result = agent.consume(
            std::span(first, static_cast<std::size_t>(bytes.size())));
        require(!result.fatalError, "client output is accepted by server state machine");
        for (const auto& outbound : result.outboundFrames) send(*peer, outbound);
        actions.insert(actions.end(), result.actions.begin(), result.actions.end());
    };
    require(waitFor([&] {
                consumeClient();
                return agent.state() == ServerSessionState::Ready &&
                       std::any_of(actions.begin(), actions.end(), [](const auto& action) {
                           return std::holds_alternative<CatalogRequestAction>(action);
                       });
            }),
            "client hello ack and initial catalog request reach server session");

    const TopicCatalog catalog{
        1,
        {{"/temperature", "std_msgs/msg/Float64", Reliability::Reliable,
          Durability::Volatile}}};
    const auto catalogFrame = agent.makeTopicCatalog(catalog);
    require(catalogFrame.has_value(), "ready Agent makes catalog");
    send(*peer, *catalogFrame);
    require(waitFor([&] {
                std::scoped_lock lock(events.mutex);
                return events.catalogs.size() == 1;
            }),
            "client accepts catalog produced by server session");
    {
        std::scoped_lock lock(events.mutex);
        require(events.fieldCatalogs.empty(),
                "client remains compatible when an older Agent does not offer a field catalog");
    }

    const SubscriptionRequest request{
        9, "/temperature", "std_msgs/msg/Float64", Reliability::Reliable, 10};
    actions.clear();
    require(source.subscribe(request), "compatibility subscription sent");
    require(waitFor([&] {
                consumeClient();
                return std::any_of(actions.begin(), actions.end(), [&](const auto& action) {
                    const auto* subscribe = std::get_if<SubscribeAction>(&action);
                    return subscribe && subscribe->request == request;
                });
            }),
            "server session exposes exact subscription action");

    const auto sampleFrame = agent.makeSample(
        {"/temperature", "std_msgs/msg/Float64", {0, 1, 2, 3}, {{"data", "C", 24.5}}},
        123,
        456);
    require(sampleFrame.has_value(), "ready Agent makes sample");
    send(*peer, *sampleFrame);
    require(waitFor([&] {
                std::scoped_lock lock(events.mutex);
                return events.chunks.size() == 1 && events.samples.size() == 1 &&
                       events.samples[0].value == 24.5;
            }),
            "client accepts CDR and field produced by server session");

    source.close();
    agent.reset();
    peer->deleteLater();
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    try {
        testIdleCloseIsSilent();
        testDisconnectBeforeHelloIsError();
        testHandshakeCatalogSamplesAndControls();
        testCapabilityAwareHandshakeWithoutTopicDiscovery();
        testActiveClockSynchronization();
        testAutomaticReconnectRestoresSubscriptions();
        testProtocolFailureDoesNotAutoReconnect();
        testClientAndServerStateMachinesAreCompatible();
        std::cout << "All Remote Agent source tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Test failed: " << exception.what() << '\n';
        return 1;
    }
}

#include "app/serial_session.hpp"

#include "lab/core/agent_protocol.hpp"
#include "lab/core/raw_log_reader.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QUdpSocket>
#include <QUuid>
#include <QVariantMap>

#include <filesystem>
#include <functional>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace lab::core;
using namespace lab::core::agent;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error("multi-source session: " + message);
    }
}

bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 3000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

QString fromPath(const std::filesystem::path& path) {
#ifdef _WIN32
    return QString::fromStdWString(path.wstring());
#else
    return QString::fromStdString(path.string());
#endif
}

std::filesystem::path currentPath() {
#ifdef _WIN32
    return std::filesystem::path(QDir::currentPath().toStdWString());
#else
    return std::filesystem::path(QDir::currentPath().toStdString());
#endif
}

quint16 reserveUdpPort() {
    QUdpSocket socket;
    require(socket.bind(QHostAddress::LocalHost, 0), "reserves UDP listen port");
    return socket.localPort();
}

void sendFrame(QTcpSocket& socket, const Frame& frame) {
    const auto encoded = encodeFrame(frame);
    const QByteArray bytes(
        reinterpret_cast<const char*>(encoded.data()),
        static_cast<qsizetype>(encoded.size()));
    require(socket.write(bytes) == bytes.size(), "Agent fixture writes complete frame");
    socket.flush();
}

QJsonObject readJsonObject(const std::filesystem::path& path) {
    QFile file(fromPath(path));
    require(file.open(QIODevice::ReadOnly), "opens metadata JSON");
    const auto document = QJsonDocument::fromJson(file.readAll());
    require(document.isObject(), "metadata is a JSON object");
    return document.object();
}

void runTest() {
    QTcpServer agentServer;
    require(agentServer.listen(QHostAddress::LocalHost, 0),
            "Agent fixture listens");

    QUdpSocket udpPeer;
    require(udpPeer.bind(QHostAddress::LocalHost, 0), "UDP fixture binds");
    const auto sessionUdpPort = reserveUdpPort();
    const auto root = currentPath() /
                      ("lab-debugger-multi-source-" +
                       QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString());
    const auto lateRoot = currentPath() /
                          ("lab-debugger-late-source-" +
                           QUuid::createUuid()
                               .toString(QUuid::WithoutBraces)
                               .toStdString());

    {
        lab::app::SerialSession session;
        session.setCsvFields({QStringLiteral("value")});

        bool networkOpen = false;
        bool remoteOpen = false;
        QObject::connect(
            &session,
            &lab::app::SerialSession::networkStateChanged,
            &session,
            [&networkOpen](int state) {
                networkOpen = state == static_cast<int>(SourceState::Open);
            });
        QObject::connect(
            &session,
            &lab::app::SerialSession::remoteAgentStateChanged,
            &session,
            [&remoteOpen](int state) {
                remoteOpen = state == static_cast<int>(SourceState::Open);
            });

        session.connectNetwork({lab::adapters::network::NetworkMode::Udp,
                                "127.0.0.1",
                                udpPeer.localPort(),
                                "127.0.0.1",
                                sessionUdpPort});
        require(waitFor([&] { return networkOpen; }), "UDP source opens");

        session.connectRemoteAgent({"127.0.0.1",
                                    agentServer.serverPort(),
                                    "Multi-source test",
                                    "0.16.0",
                                    false});
        require(waitFor([&] { return agentServer.hasPendingConnections(); }),
                "Remote Agent fixture accepts client");
        auto* agentPeer = agentServer.nextPendingConnection();
        require(agentPeer != nullptr, "Remote Agent peer exists");

        const auto capabilities = capabilityMask(Capability::TopicDiscovery) |
                                  capabilityMask(Capability::SerializedMessages) |
                                  capabilityMask(Capability::NumericFields);
        sendFrame(*agentPeer,
                  {MessageType::Hello,
                   0,
                   10,
                   1'000'000,
                   1'000'100,
                   encodeHello({"multi-agent", "0.10.0", "fixture", capabilities})});
        require(waitFor([&] { return remoteOpen; }),
                "Remote Agent handshake reaches ready state");

        require(session.startSession(fromPath(root)),
                "recording starts with two live sources");

        require(udpPeer.writeDatagram(
                    "42\n", QHostAddress::LocalHost, sessionUdpPort) == 3,
                "UDP sample is sent");
        const SampleBatch batch{
            "/temperature",
            "std_msgs/msg/Float64",
            {0x00, 0x01, 0x00, 0x00, 0xCD, 0xAB},
            {{"data", "degC", 36.5}}};
        sendFrame(*agentPeer,
                  {MessageType::SampleBatch,
                   0,
                   11,
                   2'000'000,
                   2'000'100,
                   encodeSampleBatch(batch)});

        const auto udpField = "udp:127.0.0.1:" +
                              std::to_string(sessionUdpPort) + ".value";
        require(waitFor([&] {
                    const auto udp = session.timeSeries().snapshot(udpField);
                    const auto remote =
                        session.timeSeries().snapshot("/temperature.data");
                    return udp.size() == 1 && udp.front().value == 42.0 &&
                           remote.size() == 1 && remote.front().value == 36.5;
                }),
                "both sources reach independent curve series");

        session.stopSession();
        session.disconnectRemoteAgent();
        agentPeer->deleteLater();
        require(waitFor([&] { return !remoteOpen; }),
                "first Agent connection closes");

        QVariantMap lateDerived;
        lateDerived.insert(QStringLiteral("name"),
                           QStringLiteral("late_temperature_double"));
        lateDerived.insert(QStringLiteral("expression"),
                           QStringLiteral("`/temperature.data` * 2"));
        lateDerived.insert(QStringLiteral("unit"), QStringLiteral("degC"));
        require(session.setDerivedFields({lateDerived}),
                "configures a derived value driven only by the late Agent");
        session.setCsvFields({QStringLiteral("value")});
        session.connectRemoteAgent({"127.0.0.1",
                                    agentServer.serverPort(),
                                    "Multi-source test",
                                    "0.16.0",
                                    false});
        require(waitFor([&] { return agentServer.hasPendingConnections(); }),
                "late Agent fixture accepts client");
        auto* lateAgentPeer = agentServer.nextPendingConnection();
        require(lateAgentPeer != nullptr, "late Agent peer exists");
        require(session.startSession(fromPath(lateRoot)),
                "recording starts while Agent handshake is pending");

        sendFrame(*lateAgentPeer,
                  {MessageType::Hello,
                   0,
                   10,
                   3'000'000,
                   3'000'100,
                   encodeHello({"late-agent", "0.10.0", "fixture", capabilities})});
        require(waitFor([&] { return remoteOpen; }),
                "late Agent becomes ready after recording starts");
        require(udpPeer.writeDatagram(
                    "43\n", QHostAddress::LocalHost, sessionUdpPort) == 3,
                "declared UDP source continues recording");
        sendFrame(*lateAgentPeer,
                  {MessageType::SampleBatch,
                   0,
                   11,
                   4'000'000,
                   4'000'100,
                   encodeSampleBatch(batch)});
        require(waitFor([&] {
                    const auto udp = session.timeSeries().snapshot(udpField);
                    const auto remote =
                        session.timeSeries().snapshot("/temperature.data");
                    return udp.size() == 1 && udp.front().value == 43.0 &&
                           remote.size() == 1;
                }),
                "late source remains visible without entering the frozen recording set");
        require(session.timeSeries()
                    .snapshot("late_temperature_double")
                    .empty(),
                "late undeclared source cannot produce a frozen Session derived value");
        session.stopSession();
        session.disconnectRemoteAgent();
        session.disconnectNetwork();
        lateAgentPeer->deleteLater();
    }

    const auto metadata = readJsonObject(root / "metadata.json");
    require(metadata.value(QStringLiteral("software_version")).toString() ==
                QStringLiteral("0.16.0"),
            "metadata records current application version");
    require(metadata.value(QStringLiteral("status")).toString() ==
                QStringLiteral("completed"),
            "recording completes cleanly");
    const auto sources = metadata.value(QStringLiteral("sources")).toArray();
    require(sources.size() == 2, "metadata declares both live sources");
    std::set<QString> sourceTypes;
    for (const auto& source : sources) {
        sourceTypes.insert(source.toObject().value(QStringLiteral("type")).toString());
    }
    require(sourceTypes == std::set<QString>{QStringLiteral("ros_remote_agent"),
                                             QStringLiteral("udp")},
            "metadata identifies UDP and Remote Agent source types");

    const auto counts = metadata.value(QStringLiteral("counts")).toObject();
    require(counts.value(QStringLiteral("raw_chunks")).toInteger() == 2,
            "both raw payloads are recorded");
    require(counts.value(QStringLiteral("samples")).toInteger() == 2,
            "both structured values are recorded");

    RawLogReader reader;
    require(reader.open(root / "raw" / "stream.ldraw"), "opens recorded raw log");
    require(reader.recordCount() == 2, "raw log contains exactly two payloads");
    std::set<std::string> rawSourceIds;
    for (std::size_t index = 0; index < reader.recordCount(); ++index) {
        const auto chunk = reader.read(index);
        require(chunk.has_value(), "reads raw log item");
        rawSourceIds.insert(chunk->sourceId);
    }
    require(rawSourceIds.contains(
                "udp:127.0.0.1:" + std::to_string(sessionUdpPort)),
            "raw log retains UDP source identity");
    require(rawSourceIds.contains("ros-agent:multi-agent:/temperature"),
            "raw log retains Agent and Topic identity");
    reader.close();

    QFile values(fromPath(root / "values.csv"));
    require(values.open(QIODevice::ReadOnly), "opens structured value log");
    const auto valueText = values.readAll();
    require(valueText.contains(QByteArray::fromStdString(
                "udp:127.0.0.1:" + std::to_string(sessionUdpPort) + ".value")) &&
                valueText.contains("/temperature.data"),
            "structured log keeps both independent curve names");
    values.close();

    const auto lateMetadata = readJsonObject(lateRoot / "metadata.json");
    const auto lateSources =
        lateMetadata.value(QStringLiteral("sources")).toArray();
    require(lateSources.size() == 1 &&
                lateSources.at(0)
                        .toObject()
                        .value(QStringLiteral("type"))
                        .toString() == QStringLiteral("udp"),
            "recording source set remains frozen while Agent handshake completes");
    const auto lateCounts =
        lateMetadata.value(QStringLiteral("counts")).toObject();
    require(lateCounts.value(QStringLiteral("raw_chunks")).toInteger() == 1 &&
                lateCounts.value(QStringLiteral("samples")).toInteger() == 1,
            "late Agent payload is excluded from raw and structured recording");
    RawLogReader lateReader;
    require(lateReader.open(lateRoot / "raw" / "stream.ldraw") &&
                lateReader.recordCount() == 1,
            "late-source recording contains only declared raw data");
    const auto lateChunk = lateReader.read(0);
    require(lateChunk && lateChunk->sourceId ==
                             "udp:127.0.0.1:" +
                                 std::to_string(sessionUdpPort),
            "late-source raw log retains only the declared UDP identity");
    lateReader.close();
    QFile lateValues(fromPath(lateRoot / "values.csv"));
    require(lateValues.open(QIODevice::ReadOnly),
            "opens late-source structured value log");
    const auto lateValueText = lateValues.readAll();
    require(lateValueText.contains(QByteArray::fromStdString(
                "udp:127.0.0.1:" + std::to_string(sessionUdpPort) + ".value")) &&
                !lateValueText.contains("/temperature.data") &&
                !lateValueText.contains("late_temperature_double"),
            "late Agent and its derived curve are not written to the frozen Session");
    lateValues.close();

    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    require(!cleanupError, "removes multi-source fixture");
    std::filesystem::remove_all(lateRoot, cleanupError);
    require(!cleanupError, "removes late-source fixture");
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    try {
        runTest();
        std::cout << "Lab Debugger multi-source session tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Lab Debugger multi-source session tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}

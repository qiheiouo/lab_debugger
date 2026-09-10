#include "app/serial_session.hpp"

#include "lab/core/checksum.hpp"
#include "lab/core/remote_agent_session.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QUuid>
#include <QVariantMap>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool waitFor(const std::function<bool()>& condition,
             std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return condition();
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
    require(socket.bind(QHostAddress::LocalHost, 0),
            "temporary UDP socket binds");
    return socket.localPort();
}

void writeFile(const std::filesystem::path& path, const QByteArray& contents) {
    QFile file(fromPath(path));
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
            "validation file opens for writing");
    require(file.write(contents) == contents.size(),
            "validation file is fully written");
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

QByteArray validFrame() {
    std::vector<std::uint8_t> bytes{0xAA, 0x55, 0x2A};
    const auto crc = lab::core::crc16Modbus(bytes);
    bytes.push_back(static_cast<std::uint8_t>(crc & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>(crc >> 8U));
    return QByteArray(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<qsizetype>(bytes.size()));
}

QVariantMap healthRule(const QString& name,
                       const QString& sourceId,
                       const QString& kind,
                       int errorCount,
                       int windowMs,
                       const QString& message) {
    QVariantMap rule;
    rule.insert(QStringLiteral("name"), name);
    rule.insert(QStringLiteral("source_id"), sourceId);
    rule.insert(QStringLiteral("kind"), kind);
    rule.insert(QStringLiteral("error_count"), errorCount);
    rule.insert(QStringLiteral("window_ms"), windowMs);
    rule.insert(QStringLiteral("message"), message);
    return rule;
}

int countRuleAlerts(const QVariantList& events, const QString& ruleName) {
    int count = 0;
    const auto marker = QStringLiteral("[") + ruleName + QStringLiteral("]");
    for (const auto& value : events) {
        const auto event = value.toMap();
        if (event.value(QStringLiteral("category")).toString() ==
                QStringLiteral("alert") &&
            event.value(QStringLiteral("message")).toString().contains(marker)) {
            ++count;
        }
    }
    return count;
}

void sendAgentBytes(QTcpSocket& socket, std::span<const std::uint8_t> bytes) {
    require(socket.write(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<qint64>(bytes.size())) ==
                static_cast<qint64>(bytes.size()) &&
                socket.waitForBytesWritten(1000),
            "fake Agent frame is sent");
}

void testRemoteAgentIdentityChange() {
    using lab::core::agent::Capability;
    using lab::core::agent::Hello;
    using lab::core::agent::Reliability;
    using lab::core::agent::ServerSession;
    using lab::core::agent::SubscriptionRequest;
    using lab::core::agent::capabilityMask;

    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost, 0),
            "identity-change Agent fixture listens");
    lab::app::SerialSession session;
    QVariantList timeline;
    QString currentAgent;
    QObject::connect(
        &session, &lab::app::SerialSession::timelineEventsChanged, &session,
        [&timeline](const QVariantList& events) { timeline = events; });
    QObject::connect(
        &session, &lab::app::SerialSession::remoteAgentHello, &session,
        [&currentAgent](const QString& agentId, const QString&, const QString&, quint32) {
            currentAgent = agentId;
        });

    constexpr int timeoutMs = 3'000;
    const QString oldAgent = QStringLiteral("old-agent");
    const QString oldBase = QStringLiteral("ros-agent:old-agent");
    const QString oldTopic = QStringLiteral("ros-agent:old-agent:/temperature");
    require(session.setHealthAlertRules(
                {healthRule(QStringLiteral("旧 Agent 无数据"), oldBase,
                            QStringLiteral("inactivity"), 1, timeoutMs, {}),
                 healthRule(QStringLiteral("旧 Topic 无数据"), oldTopic,
                            QStringLiteral("inactivity"), 1, timeoutMs, {})}),
            "old Agent identity health rules configure");
    session.connectRemoteAgent(
        {"127.0.0.1", server.serverPort(), "Health test", "0.21.0", true});
    require(waitFor([&] { return server.hasPendingConnections(); }),
            "first Agent transport connects");
    auto* firstPeer = server.nextPendingConnection();
    const auto capabilities = capabilityMask(Capability::TopicDiscovery) |
                              capabilityMask(Capability::SerializedMessages) |
                              capabilityMask(Capability::NumericFields);
    ServerSession firstAgent(
        Hello{oldAgent.toStdString(), "0.10.0", "fixture", capabilities});
    const auto firstHello = firstAgent.start();
    sendAgentBytes(*firstPeer, firstHello);
    require(waitFor([&] { return currentAgent == oldAgent; }),
            "first Agent identity completes the handshake");
    session.subscribeRemoteTopic(
        SubscriptionRequest{1, "/temperature", "std_msgs/msg/Float64",
                            Reliability::Reliable, 10});

    firstPeer->disconnectFromHost();
    require(waitFor([&] { return server.hasPendingConnections(); },
                    std::chrono::milliseconds(2500)),
            "client reconnects to the same endpoint");
    firstPeer->deleteLater();
    auto* secondPeer = server.nextPendingConnection();
    ServerSession secondAgent(
        Hello{"new-agent", "0.10.0", "fixture", capabilities});
    const auto secondHello = secondAgent.start();
    sendAgentBytes(*secondPeer, secondHello);
    require(waitFor([&] { return currentAgent == QStringLiteral("new-agent"); }),
            "reconnected endpoint reports its new Agent identity");
    require(countRuleAlerts(timeline, QStringLiteral("旧 Agent 无数据")) == 0 &&
                countRuleAlerts(timeline, QStringLiteral("旧 Topic 无数据")) == 0,
            "old identity does not expire before replacement handshake");

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs + 150);
    while (std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    require(countRuleAlerts(timeline, QStringLiteral("旧 Agent 无数据")) == 0 &&
                countRuleAlerts(timeline, QStringLiteral("旧 Topic 无数据")) == 0,
            "new Agent handshake disarms stale Agent and Topic health timers");
    session.disconnectRemoteAgent();
    secondPeer->deleteLater();
}

int runRealRemoteHealthProbe(quint16 port) {
    const QString agentId = QStringLiteral("linux-validation-021");
    const QString agentSource = QStringLiteral("ros-agent:") + agentId;
    const QString floatTopic = QStringLiteral("/lab_health_float64");
    const QString rawTopic = QStringLiteral("/lab_health_raw");
    const QString floatSource = agentSource + QStringLiteral(":") + floatTopic;
    const QString rawSource = agentSource + QStringLiteral(":") + rawTopic;
    constexpr int timeoutMs = 500;

    lab::app::SerialSession session;
    QVariantList timeline;
    int helloCount = 0;
    QString receivedAgentId;
    QStringList liveFields;
    QObject::connect(
        &session, &lab::app::SerialSession::timelineEventsChanged, &session,
        [&timeline](const QVariantList& events) { timeline = events; });
    QObject::connect(
        &session, &lab::app::SerialSession::remoteAgentHello, &session,
        [&helloCount, &receivedAgentId](const QString& value, const QString&,
                                       const QString&, quint32) {
            receivedAgentId = value;
            ++helloCount;
        });
    QObject::connect(
        &session, &lab::app::SerialSession::liveFieldsDiscovered, &session,
        [&liveFields](const QStringList& fields) { liveFields = fields; });
    require(session.setHealthAlertRules(
                {healthRule(QStringLiteral("Agent 无数据"), agentSource,
                            QStringLiteral("inactivity"), 1, timeoutMs, {}),
                 healthRule(QStringLiteral("Float64 无数据"), floatSource,
                            QStringLiteral("inactivity"), 1, timeoutMs, {}),
                 healthRule(QStringLiteral("RawOnly 无数据"), rawSource,
                            QStringLiteral("inactivity"), 1, timeoutMs, {})}),
            "real Agent health rules configure");
    session.connectRemoteAgent(
        {"127.0.0.1", port, "Health probe", "0.21.0", true});
    require(waitFor([&] { return helloCount == 1; }, std::chrono::seconds(5)),
            "real Agent handshake completes");
    require(receivedAgentId == agentId,
            "real Agent reports the expected identity");
    session.subscribeRemoteTopic(
        {1, floatTopic.toStdString(), "std_msgs/msg/Float64",
         lab::core::agent::Reliability::Reliable, 10});
    session.subscribeRemoteTopic(
        {2, rawTopic.toStdString(), "std_msgs/msg/String",
         lab::core::agent::Reliability::Reliable, 10});
    const auto field = floatTopic.toStdString() + ".data";
    if (!waitFor([&] { return session.timeSeries().snapshot(field).size() >= 3; },
                 std::chrono::seconds(5))) {
        std::ostringstream detail;
        detail << "real Float64 SampleBatch reaches the structured curve; observed fields:";
        for (const auto& value : liveFields) {
            detail << ' ' << value.toStdString();
        }
        throw std::runtime_error(detail.str());
    }
    require(countRuleAlerts(timeline, QStringLiteral("Agent 无数据")) == 0 &&
                countRuleAlerts(timeline, QStringLiteral("Float64 无数据")) == 0 &&
                countRuleAlerts(timeline, QStringLiteral("RawOnly 无数据")) == 0,
            "active structured and raw-only SampleBatch traffic prevents timeouts");

    require(waitFor([&] {
                return countRuleAlerts(timeline, QStringLiteral("Float64 无数据")) == 1 &&
                       countRuleAlerts(timeline, QStringLiteral("RawOnly 无数据")) == 1;
            }, std::chrono::seconds(5)),
            "first publisher stop triggers one alert per Topic");
    const auto samplesAfterFirstStop = session.timeSeries().snapshot(field).size();
    require(waitFor([&] {
                return session.timeSeries().snapshot(field).size() > samplesAfterFirstStop;
            }, std::chrono::seconds(5)),
            "publisher recovery restores real SampleBatch traffic");
    require(waitFor([&] {
                return countRuleAlerts(timeline, QStringLiteral("Float64 无数据")) == 2 &&
                       countRuleAlerts(timeline, QStringLiteral("RawOnly 无数据")) == 2;
            }, std::chrono::seconds(5)),
            "second publisher stop triggers a second alert after recovery");

    session.unsubscribeRemoteTopic(
        {3, floatTopic.toStdString(), "std_msgs/msg/Float64",
         lab::core::agent::Reliability::Reliable, 10});
    session.unsubscribeRemoteTopic(
        {4, rawTopic.toStdString(), "std_msgs/msg/String",
         lab::core::agent::Reliability::Reliable, 10});
    const auto quietDeadline = std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(timeoutMs + 250);
    while (std::chrono::steady_clock::now() < quietDeadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    require(countRuleAlerts(timeline, QStringLiteral("Float64 无数据")) == 2 &&
                countRuleAlerts(timeline, QStringLiteral("RawOnly 无数据")) == 2,
            "unsubscribe disarms both Topic timeout rules");

    session.subscribeRemoteTopic(
        {5, floatTopic.toStdString(), "std_msgs/msg/Float64",
         lab::core::agent::Reliability::Reliable, 10});
    session.subscribeRemoteTopic(
        {6, rawTopic.toStdString(), "std_msgs/msg/String",
         lab::core::agent::Reliability::Reliable, 10});
    require(waitFor([&] {
                return countRuleAlerts(timeline, QStringLiteral("Float64 无数据")) == 3 &&
                       countRuleAlerts(timeline, QStringLiteral("RawOnly 无数据")) == 3;
            }, std::chrono::seconds(3)),
            "resubscribe rearms both Topic timeout rules");

    const auto samplesBeforeReconnect = session.timeSeries().snapshot(field).size();
    session.disconnectRemoteAgent();
    session.reconnectRemoteAgent();
    require(waitFor([&] { return helloCount == 2; }, std::chrono::seconds(5)),
            "real Agent reconnect handshake completes");
    require(receivedAgentId == agentId,
            "reconnected Agent preserves the expected identity");
    require(waitFor([&] {
                return session.timeSeries().snapshot(field).size() > samplesBeforeReconnect;
            }, std::chrono::seconds(6)),
            "restored subscription receives samples under the same exact Agent identity");
    session.disconnectRemoteAgent();
    std::cout << "Real ROS2 Remote Agent health probe passed.\n";
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    if (argc == 3 && std::string_view(argv[1]) == "--real-agent") {
        try {
            const auto port = std::stoul(argv[2]);
            require(port > 0 && port <= 65'535, "real Agent port is valid");
            return runRealRemoteHealthProbe(static_cast<quint16>(port));
        } catch (const std::exception& exception) {
            std::cerr << "Real ROS2 Remote Agent health probe failed: "
                      << exception.what() << '\n';
            return 1;
        }
    }
    std::error_code cleanupError;
    const auto suffix =
        QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    const auto root = currentPath() / ("lab-debugger-health-alerts-" + suffix);
    const auto protocolPath =
        currentPath() / ("lab-debugger-health-alerts-" + suffix + ".json");
    try {
        writeFile(protocolPath, R"json({
  "name": "health_probe",
  "frame": {"header": ["0xAA", "0x55"], "length": 5},
  "fields": [
    {"name": "value", "type": "uint8", "byte_offset": 2, "unit": "count"}
  ],
  "checksum": {
    "type": "crc16_modbus",
    "offset": 3,
    "range_start": 0,
    "range_length": 3,
    "endian": "little"
  }
})json");

        QUdpSocket peer;
        require(peer.bind(QHostAddress::LocalHost, 0), "UDP peer binds");
        const auto sessionPort = reserveUdpPort();
        const auto sourceId =
            QStringLiteral("udp:127.0.0.1:%1").arg(sessionPort);

        lab::app::SerialSession session;
        bool networkOpen = false;
        QVariantList timeline;
        QVariantList restoredHealthRules;
        QString replayFailure;
        QObject::connect(
            &session, &lab::app::SerialSession::networkStateChanged, &session,
            [&networkOpen](int state) {
                networkOpen =
                    state == static_cast<int>(lab::core::SourceState::Open);
            });
        QObject::connect(
            &session, &lab::app::SerialSession::timelineEventsChanged, &session,
            [&timeline](const QVariantList& events) { timeline = events; });
        QObject::connect(
            &session, &lab::app::SerialSession::healthAlertRulesRestored,
            &session, [&restoredHealthRules](const QVariantList& definitions) {
                restoredHealthRules = definitions;
            });
        QObject::connect(
            &session, &lab::app::SerialSession::replayOpenFailed, &session,
            [&replayFailure](const QString& message) { replayFailure = message; });

        const QVariantList rules{
            healthRule(QStringLiteral("协议错误过多"), sourceId,
                       QStringLiteral("error_rate"), 2, 500,
                       QStringLiteral("检查链路质量")),
            healthRule(QStringLiteral("来源无数据"), sourceId,
                       QStringLiteral("inactivity"), 1, 350,
                       QStringLiteral("检查设备连接"))};
        require(session.setHealthAlertRules(rules),
                "valid exact-source health rules are accepted");
        session.connectNetwork({lab::adapters::network::NetworkMode::Udp,
                                "127.0.0.1",
                                peer.localPort(),
                                "127.0.0.1",
                                sessionPort});
        require(waitFor([&] { return networkOpen; }), "UDP source opens");
        require(session.loadSourceProtocolFile(sourceId, fromPath(protocolPath)),
                "source-specific CRC protocol loads");
        require(session.startSession(fromPath(root)), "session starts");
        require(!session.setHealthAlertRules({}),
                "recording freezes health alert definitions");

        const auto send = [&](const QByteArray& payload) {
            require(peer.writeDatagram(payload, QHostAddress::LocalHost,
                                       sessionPort) == payload.size(),
                    "UDP frame is sent");
        };
        const auto good = validFrame();
        auto damaged = good;
        damaged[3] = static_cast<char>(damaged[3] ^ 0x5A);
        send(good);
        send(damaged);
        send(damaged);
        require(waitFor([&] {
                    return countRuleAlerts(timeline,
                                           QStringLiteral("协议错误过多")) == 1;
                }),
                "two CRC failures inside the window trigger one error-rate alert");
        require(waitFor([&] {
                    return countRuleAlerts(timeline,
                                           QStringLiteral("来源无数据")) == 1;
                }),
                "lack of data after an open source triggers an inactivity alert");
        const auto alertCount = timeline.size();
        QThread::msleep(120);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        require(timeline.size() == alertCount,
                "latched health conditions do not spam the timeline");

        send(good);
        require(waitFor([&] {
                    return session.timeSeries()
                               .snapshot(sourceId.toStdString() + ".value")
                               .size() == 2;
                }),
                "new activity recovers the inactivity condition");
        require(waitFor([&] {
                    return countRuleAlerts(timeline,
                                           QStringLiteral("来源无数据")) == 2;
                }),
                "a recovered source can later trigger inactivity again");

        require(waitFor([&] {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                    return countRuleAlerts(timeline,
                                           QStringLiteral("协议错误过多")) == 1;
                },
                std::chrono::milliseconds(550)),
                "expired errors leave the first error alert count unchanged");
        QThread::msleep(520);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        send(damaged);
        send(damaged);
        require(waitFor([&] {
                    return countRuleAlerts(timeline,
                                           QStringLiteral("协议错误过多")) == 2;
                }),
                "error-rate alert rearms after its rolling window recovers");
        session.stopSession();
        session.disconnectNetwork();

        const auto healthConfiguration = readText(
            root / "configuration" / "health_alert_rules.json");
        const auto healthDocument = QJsonDocument::fromJson(
            QByteArray::fromStdString(healthConfiguration));
        require(healthDocument.isObject() &&
                    healthDocument.object()
                            .value(QStringLiteral("format_version"))
                            .toInt() == 1 &&
                    healthDocument.object()
                            .value(QStringLiteral("rules"))
                            .toArray()
                            .size() == 2,
                "health rules are stored in a versioned session snapshot");
        const auto events = readText(root / "events.jsonl");
        require(events.find("协议错误过多") != std::string::npos &&
                    events.find("来源无数据") != std::string::npos,
                "health alerts are persisted in the session event log");

        timeline.clear();
        require(session.openReplaySession(fromPath(root)),
                "health alert session reopens");
        require(restoredHealthRules.size() == 2,
                "replay restores health rules into the UI model");
        require(countRuleAlerts(timeline, QStringLiteral("协议错误过多")) == 2 &&
                    countRuleAlerts(timeline, QStringLiteral("来源无数据")) == 2,
                "replay restores recorded health alerts without regenerating them");
        session.closeReplay();

        const auto healthPath =
            root / "configuration" / "health_alert_rules.json";
        writeFile(healthPath,
                  QByteArrayLiteral("{\"format_version\":999,\"rules\":[]}"));
        replayFailure.clear();
        require(!session.openReplaySession(fromPath(root)) &&
                    !replayFailure.isEmpty(),
                "future health alert configuration is rejected explicitly");
        writeFile(
            healthPath,
            QByteArrayLiteral(
                "{\"format_version\":1,\"rules\":[{\"name\":\"bad\","
                "\"source_id\":\"udp:x:1\",\"kind\":\"error_rate\","
                "\"error_count\":1.5,\"window_ms\":100,"
                "\"message\":\"bad\"}]}"));
        replayFailure.clear();
        require(!session.openReplaySession(fromPath(root)) &&
                    !replayFailure.isEmpty(),
                "fractional error counts are rejected instead of rounded");
        writeFile(healthPath, QByteArrayLiteral("{broken"));
        replayFailure.clear();
        require(!session.openReplaySession(fromPath(root)) &&
                    !replayFailure.isEmpty(),
                "damaged health alert JSON is rejected explicitly");
        writeFile(healthPath, QByteArray(1024 * 1024 + 1, ' '));
        replayFailure.clear();
        require(!session.openReplaySession(fromPath(root)) &&
                    !replayFailure.isEmpty(),
                "oversized health alert configuration is rejected before parsing");
        writeFile(healthPath, QByteArray::fromStdString(healthConfiguration));

        testRemoteAgentIdentityChange();

        std::filesystem::remove(protocolPath, cleanupError);
        std::filesystem::remove_all(root, cleanupError);
        std::cout << "Lab Debugger health alert integration tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::filesystem::remove(protocolPath, cleanupError);
        std::filesystem::remove_all(root, cleanupError);
        std::cerr << "Lab Debugger health alert integration tests failed: "
                  << exception.what() << '\n';
        return 1;
    }
}

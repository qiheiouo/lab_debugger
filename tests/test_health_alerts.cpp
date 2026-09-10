#include "app/serial_session.hpp"

#include "lab/core/checksum.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
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

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
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
        writeFile(healthPath, QByteArray::fromStdString(healthConfiguration));

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

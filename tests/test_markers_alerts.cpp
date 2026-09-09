#include "app/serial_session.hpp"

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
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

bool waitFor(const std::function<bool()> &condition,
             std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return condition();
}

QString fromPath(const std::filesystem::path &path) {
#ifdef _WIN32
    return QString::fromStdWString(path.wstring());
#else
    return QString::fromStdString(path.string());
#endif
}

std::filesystem::path toPath(const QString &path) {
#ifdef _WIN32
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toStdString());
#endif
}

quint16 reserveUdpPort() {
    QUdpSocket socket;
    require(socket.bind(QHostAddress::LocalHost, 0), "temporary UDP socket binds");
    return socket.localPort();
}

std::string readText(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void writeText(const std::filesystem::path &path, const std::string &text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "opens validation file for writing");
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    require(static_cast<bool>(output), "writes complete validation file");
}

QVariantList overheatRules(quint16 port) {
    QVariantMap rule;
    rule.insert(QStringLiteral("name"), QStringLiteral("温度过高"));
    rule.insert(QStringLiteral("field"), QStringLiteral("udp:127.0.0.1:%1.temperature").arg(port));
    rule.insert(QStringLiteral("comparison"), QStringLiteral("above"));
    rule.insert(QStringLiteral("threshold"), 80.0);
    rule.insert(QStringLiteral("hysteresis"), 5.0);
    rule.insert(QStringLiteral("message"), QStringLiteral("检查散热"));
    QVariantMap derivedRule;
    derivedRule.insert(QStringLiteral("name"), QStringLiteral("派生温度过高"));
    derivedRule.insert(QStringLiteral("field"), QStringLiteral("hot_index"));
    derivedRule.insert(QStringLiteral("comparison"), QStringLiteral("above"));
    derivedRule.insert(QStringLiteral("threshold"), 80.0);
    derivedRule.insert(QStringLiteral("hysteresis"), 5.0);
    derivedRule.insert(QStringLiteral("message"), QStringLiteral("检查派生指标"));
    return {rule, derivedRule};
}

int countCategory(const QVariantList &events, const QString &category) {
    int result = 0;
    for (const auto &value : events) {
        if (value.toMap().value(QStringLiteral("category")).toString() == category) {
            ++result;
        }
    }
    return result;
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication application(argc, argv);
    std::error_code cleanupError;
    const auto root = toPath(QDir::currentPath()) /
                      ("lab-debugger-markers-alerts-" +
                       QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString());
    try {
        QUdpSocket peer;
        require(peer.bind(QHostAddress::LocalHost, 0), "UDP peer binds");
        const auto sessionPort = reserveUdpPort();

        lab::app::SerialSession session;
        bool networkOpen = false;
        QVariantList timeline;
        QVariantList restoredRules;
        QString replayFailure;
        QObject::connect(&session, &lab::app::SerialSession::networkStateChanged, &session,
                         [&networkOpen](int state) {
                             networkOpen = state == static_cast<int>(lab::core::SourceState::Open);
                         });
        QObject::connect(&session, &lab::app::SerialSession::timelineEventsChanged, &session,
                         [&timeline](const QVariantList &events) { timeline = events; });
        QObject::connect(
            &session, &lab::app::SerialSession::alertRulesRestored, &session,
            [&restoredRules](const QVariantList &definitions) { restoredRules = definitions; });
        QObject::connect(&session, &lab::app::SerialSession::replayOpenFailed, &session,
                         [&replayFailure](const QString &message) { replayFailure = message; });

        session.setCsvFields({QStringLiteral("temperature")});
        QVariantMap derived;
        derived.insert(QStringLiteral("name"), QStringLiteral("hot_index"));
        derived.insert(QStringLiteral("expression"),
                       QStringLiteral("`udp:127.0.0.1:%1.temperature`").arg(sessionPort));
        derived.insert(QStringLiteral("unit"), QStringLiteral("C"));
        require(session.setDerivedFields({derived}), "derived alert input is accepted");
        require(session.setAlertRules(overheatRules(sessionPort)),
                "valid threshold rule is accepted");
        session.connectNetwork({lab::adapters::network::NetworkMode::Udp, "127.0.0.1",
                                peer.localPort(), "127.0.0.1", sessionPort});
        require(waitFor([&] { return networkOpen; }), "UDP source opens");
        require(session.startSession(fromPath(root)), "session starts");
        require(!session.setAlertRules({}), "recording freezes alert definitions");
        require(session.addManualMarker(QStringLiteral("开始旋转测试")),
                "manual marker is accepted while recording");

        const auto send = [&](const QByteArray &payload) {
            require(peer.writeDatagram(payload, QHostAddress::LocalHost, sessionPort) ==
                        payload.size(),
                    "UDP row is sent");
        };
        send("79\n");
        send("81\n");
        require(waitFor([&] { return countCategory(timeline, QStringLiteral("alert")) == 2; }),
                "first threshold crossing produces base and derived alert markers");
        send("90\n");
        send("76\n");
        QThread::msleep(30);
        QCoreApplication::processEvents();
        require(countCategory(timeline, QStringLiteral("alert")) == 2,
                "sustained breach and hysteresis band do not spam alerts");
        send("75\n");
        send("82\n");
        require(waitFor([&] { return countCategory(timeline, QStringLiteral("alert")) == 4; }),
                "hysteresis recovery permits a later alert");
        require(countCategory(timeline, QStringLiteral("marker")) == 1,
                "manual and automatic markers share the timeline");
        session.stopSession();

        const auto events = readText(root / "events.jsonl");
        require(events.find("\"category\":\"marker\"") != std::string::npos &&
                    events.find("开始旋转测试") != std::string::npos &&
                    events.find("\"category\":\"alert\"") != std::string::npos &&
                    events.find("检查散热") != std::string::npos,
                "manual markers and alerts are persisted in the event log");
        const auto alertConfiguration = readText(root / "configuration" / "alert_rules.json");
        const auto alertDocument =
            QJsonDocument::fromJson(QByteArray::fromStdString(alertConfiguration));
        require(alertDocument.isObject() &&
                    alertDocument.object().value(QStringLiteral("format_version")).toInt() == 1 &&
                    alertDocument.object().value(QStringLiteral("rules")).toArray().size() == 2,
                "threshold rule snapshot is stored with an explicit format version");

        timeline.clear();
        require(session.openReplaySession(fromPath(root)), "marker session reopens");
        require(restoredRules.size() == 2 &&
                    restoredRules.front().toMap().value(QStringLiteral("name")).toString() ==
                        QStringLiteral("温度过高"),
                "replay restores the threshold rule into the UI model");
        require(countCategory(timeline, QStringLiteral("marker")) == 1 &&
                    countCategory(timeline, QStringLiteral("alert")) == 4,
                "replay immediately restores manual and automatic timeline markers");
        require(!session.addManualMarker(QStringLiteral("不应写入")),
                "replay timeline remains read only");
        networkOpen = false;
        session.connectNetwork({lab::adapters::network::NetworkMode::Udp, "127.0.0.1",
                                peer.localPort(), "127.0.0.1", sessionPort});
        require(waitFor([&] { return networkOpen; }),
                "live UDP source opens directly from replay mode");
        require(timeline.empty(), "switching to a live source clears replay markers");
        require(session.addManualMarker(QStringLiteral("重新进入实时模式")) &&
                    countCategory(timeline, QStringLiteral("marker")) == 1,
                "manual markers are writable after a direct replay-to-live switch");
        send("81\n");
        require(waitFor([&] { return countCategory(timeline, QStringLiteral("alert")) == 2; }),
                "alert evaluation resumes after a direct replay-to-live switch");
        session.disconnectNetwork();

        const auto alertPath = root / "configuration" / "alert_rules.json";
        writeText(alertPath, "{\"format_version\":999,\"rules\":[]}");
        replayFailure.clear();
        require(!session.openReplaySession(fromPath(root)) && !replayFailure.isEmpty(),
                "future alert configuration is rejected explicitly");
        writeText(alertPath, alertConfiguration);
        const auto eventPath = root / "events.jsonl";
        const auto eventBackup = events;
        writeText(eventPath, "{not-json\n");
        replayFailure.clear();
        require(!session.openReplaySession(fromPath(root)) && !replayFailure.isEmpty(),
                "damaged event log is rejected explicitly");
        writeText(eventPath, eventBackup);

        std::filesystem::remove_all(root, cleanupError);
        std::cout << "Lab Debugger marker and alert integration tests passed.\n";
        return 0;
    } catch (const std::exception &exception) {
        std::filesystem::remove_all(root, cleanupError);
        std::cerr << "Lab Debugger marker and alert integration tests failed: " << exception.what()
                  << '\n';
        return 1;
    }
}

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
#include <functional>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool waitFor(const std::function<bool()>& condition,
             std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
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

std::filesystem::path toPath(const QString& path) {
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

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "opens derived configuration for writing");
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    require(static_cast<bool>(output), "writes complete derived configuration");
}

QVariantList powerDefinition(quint16 port) {
    const auto source = QStringLiteral("udp:127.0.0.1:%1").arg(port);
    QVariantMap definition;
    definition.insert(QStringLiteral("name"), QStringLiteral("power"));
    definition.insert(
        QStringLiteral("expression"),
        QStringLiteral("`%1.voltage` * `%1.current`").arg(source));
    definition.insert(QStringLiteral("unit"), QStringLiteral("W"));
    return {definition};
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    std::error_code cleanupError;
    const auto root = toPath(QDir::currentPath()) /
                      ("lab-debugger-derived-fields-" +
                       QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString());
    try {
        QUdpSocket peer;
        require(peer.bind(QHostAddress::LocalHost, 0), "UDP peer binds");
        const auto sessionPort = reserveUdpPort();

        lab::app::SerialSession session;
        bool networkOpen = false;
        QObject::connect(
            &session,
            &lab::app::SerialSession::networkStateChanged,
            &session,
            [&networkOpen](int state) {
                networkOpen = state == static_cast<int>(lab::core::SourceState::Open);
            });
        session.setCsvFields(
            {QStringLiteral("voltage"), QStringLiteral("current")});
        require(session.setDerivedFields(powerDefinition(sessionPort)),
                "valid derived definition is accepted");
        session.connectNetwork({lab::adapters::network::NetworkMode::Udp,
                                "127.0.0.1",
                                peer.localPort(),
                                "127.0.0.1",
                                sessionPort});
        require(waitFor([&] { return networkOpen; }), "UDP source opens");
        require(session.startSession(fromPath(root)), "session starts");

        QVariantMap replacement;
        replacement.insert(QStringLiteral("name"), QStringLiteral("changed"));
        replacement.insert(QStringLiteral("expression"), QStringLiteral("1"));
        require(!session.setDerivedFields({replacement}),
                "recording freezes derived definitions");

        const QByteArray row("24,2\n");
        require(peer.writeDatagram(row, QHostAddress::LocalHost, sessionPort) == row.size(),
                "UDP row is sent");
        require(waitFor([&] {
                    const auto points = session.timeSeries().snapshot("power");
                    return points.size() == 1 && points.front().value == 48.0;
                }),
                "derived sample reaches the shared time-series store");
        session.stopSession();

        const auto values = readText(root / "values.csv");
        require(values.find("power,48,W") != std::string::npos,
                "derived sample is recorded as a normal session value");
        const auto configuration = readText(
            root / "configuration" / "derived_fields.json");
        const auto configurationDocument =
            QJsonDocument::fromJson(QByteArray::fromStdString(configuration));
        const auto configuredFields =
            configurationDocument.object().value(QStringLiteral("fields")).toArray();
        require(configurationDocument.isObject() && configuredFields.size() == 1,
                "session writes one derived definition in a JSON object");
        const auto configuredPower = configuredFields.at(0).toObject();
        const auto expectedPower = powerDefinition(sessionPort).front().toMap();
        require(configurationDocument.object()
                        .value(QStringLiteral("format_version"))
                        .toInt() == 1 &&
                    configuredPower.value(QStringLiteral("name")).toString() ==
                        QStringLiteral("power") &&
                    configuredPower.value(QStringLiteral("expression")).toString() ==
                        expectedPower.value(QStringLiteral("expression")).toString() &&
                    configuredPower.value(QStringLiteral("unit")).toString() ==
                        QStringLiteral("W"),
                "session preserves the exact derived name, expression, and unit");

        QVariantList restored;
        int restoreSignals = 0;
        QObject::connect(
            &session,
            &lab::app::SerialSession::derivedFieldsRestored,
            &session,
            [&restored, &restoreSignals](const QVariantList& definitions) {
                restored = definitions;
                ++restoreSignals;
            });
        QString replayFailure;
        QObject::connect(
            &session,
            &lab::app::SerialSession::replayOpenFailed,
            &session,
            [&replayFailure](const QString& message) { replayFailure = message; });
        bool atEnd = false;
        QObject::connect(
            &session,
            &lab::app::SerialSession::replayStatusChanged,
            &session,
            [&atEnd](bool, bool, bool end, double, quint64, quint64,
                     qint64, qint64, qint64) { atEnd = end; });
        require(session.openReplaySession(fromPath(root)), "recorded session reopens");
        require(restored.size() == 1 &&
                    restored.front().toMap().value(QStringLiteral("name")).toString() ==
                        QStringLiteral("power"),
                "replay restores derived definitions into the UI model");
        session.setReplaySpeed(10.0);
        session.resumeReplay();
        require(waitFor([&] { return atEnd; }), "derived session replay reaches the end");
        require(waitFor([&] {
                    const auto points = session.timeSeries().snapshot("power");
                    return points.size() == 1 && points.front().value == 48.0;
                }),
                "replay deterministically recreates the derived curve");
        session.seekReplay(0.0);
        require(session.timeSeries().snapshot("power").empty(),
                "replay seek clears derived curves and their previous input state");
        session.closeReplay();

        const auto derivedPath = root / "configuration" / "derived_fields.json";
        const auto derivedBackup = root / "configuration" / "derived_fields.backup";
        std::filesystem::rename(derivedPath, derivedBackup);
        restored = powerDefinition(sessionPort);
        const auto signalsBeforeLegacyOpen = restoreSignals;
        require(session.openReplaySession(fromPath(root)),
                "legacy Session without derived configuration opens normally");
        require(restoreSignals == signalsBeforeLegacyOpen + 1 && restored.empty(),
                "legacy Session restores an explicit empty derived definition set");
        session.closeReplay();
        std::filesystem::rename(derivedBackup, derivedPath);

        require(session.setDerivedFields(powerDefinition(sessionPort)),
                "valid derived configuration is active before unsafe replay attempts");
        writeText(derivedPath, "{\"format_version\":999,\"fields\":[]}");
        replayFailure.clear();
        require(!session.openReplaySession(fromPath(root)) && !replayFailure.isEmpty(),
                "unsupported derived configuration version is rejected explicitly");
        writeText(derivedPath, "{not-json");
        replayFailure.clear();
        require(!session.openReplaySession(fromPath(root)) && !replayFailure.isEmpty(),
                "damaged derived configuration is rejected explicitly");
        writeText(
            derivedPath,
            "{\"format_version\":1,\"fields\":[{\"name\":\"unsafe\","
            "\"expression\":\"system(1)\",\"unit\":\"\"}]}");
        replayFailure.clear();
        require(!session.openReplaySession(fromPath(root)) && !replayFailure.isEmpty(),
                "unsafe derived expression is rejected explicitly");
        writeText(derivedPath, std::string(1024 * 1024 + 1, ' '));
        replayFailure.clear();
        require(!session.openReplaySession(fromPath(root)) && !replayFailure.isEmpty(),
                "oversized derived configuration is rejected before parsing");
        writeText(derivedPath, configuration);

        networkOpen = false;
        session.connectNetwork({lab::adapters::network::NetworkMode::Udp,
                                "127.0.0.1",
                                peer.localPort(),
                                "127.0.0.1",
                                sessionPort});
        require(waitFor([&] { return networkOpen; }),
                "UDP source reopens after unsafe replay attempts");
        require(peer.writeDatagram(row, QHostAddress::LocalHost, sessionPort) == row.size(),
                "UDP row is resent after unsafe replay attempts");
        require(waitFor([&] {
                    const auto points = session.timeSeries().snapshot("power");
                    return points.size() == 1 && points.front().value == 48.0;
                }),
                "rejected Session configurations do not replace the valid definition");
        session.disconnectNetwork();

        std::filesystem::remove_all(root, cleanupError);
        std::cout << "Lab Debugger derived field integration tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::filesystem::remove_all(root, cleanupError);
        std::cerr << "Lab Debugger derived field integration tests failed: "
                  << exception.what() << '\n';
        return 1;
    }
}

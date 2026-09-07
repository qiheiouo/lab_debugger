#include "app/serial_session.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
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
        require(configuration.find("udp:127.0.0.1:") != std::string::npos &&
                    configuration.find("\"name\": \"power\"") != std::string::npos,
                "session preserves the exact derived expression");

        QVariantList restored;
        QObject::connect(
            &session,
            &lab::app::SerialSession::derivedFieldsRestored,
            &session,
            [&restored](const QVariantList& definitions) { restored = definitions; });
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
        session.closeReplay();
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

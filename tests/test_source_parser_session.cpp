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

#include <chrono>
#include <filesystem>
#include <functional>
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

std::filesystem::path currentPath() {
#ifdef _WIN32
    return std::filesystem::path(QDir::currentPath().toStdWString());
#else
    return std::filesystem::path(QDir::currentPath().toStdString());
#endif
}

quint16 reserveUdpPort() {
    QUdpSocket socket;
    require(socket.bind(QHostAddress::LocalHost, 0), "temporary UDP socket binds");
    return socket.localPort();
}

QByteArray readFile(const std::filesystem::path& path) {
    QFile file(fromPath(path));
    require(file.open(QIODevice::ReadOnly), "validation file opens");
    return file.readAll();
}

void writeFile(const std::filesystem::path& path, const QByteArray& contents) {
    QFile file(fromPath(path));
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
            "validation file opens for writing");
    require(file.write(contents) == contents.size(), "validation file is fully written");
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    std::error_code cleanupError;
    const auto suffix = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    const auto root = currentPath() / ("lab-debugger-source-parser-" + suffix);
    const auto protocolPath = currentPath() / ("lab-debugger-source-parser-" + suffix + ".json");
    try {
        writeFile(protocolPath,
                  R"({
  "name": "byte_value",
  "frame": {"header": ["0xAA"], "length": 2},
  "fields": [
    {"name": "value", "type": "uint8", "byte_offset": 1, "unit": "count"}
  ],
  "checksum": {"type": "none"}
})");

        QUdpSocket peer;
        require(peer.bind(QHostAddress::LocalHost, 0), "UDP peer binds");
        const auto sessionPort = reserveUdpPort();
        const auto sourceId = QStringLiteral("udp:127.0.0.1:%1").arg(sessionPort);

        lab::app::SerialSession session;
        bool networkOpen = false;
        QString configuredMode;
        QString configuredSource;
        QString replayFailure;
        QObject::connect(&session, &lab::app::SerialSession::networkStateChanged, &session,
                         [&networkOpen](int state) {
                             networkOpen = state ==
                                           static_cast<int>(lab::core::SourceState::Open);
                         });
        QObject::connect(
            &session, &lab::app::SerialSession::sourceParserConfigured, &session,
            [&](const QString& source, const QString& mode, const QString&, const QStringList&) {
                if (!source.isEmpty()) {
                    configuredSource = source;
                    configuredMode = mode;
                }
            });
        QObject::connect(&session, &lab::app::SerialSession::replayOpenFailed, &session,
                         [&replayFailure](const QString& message) { replayFailure = message; });

        session.setCsvFields({QStringLiteral("default_value")});
        session.connectNetwork({lab::adapters::network::NetworkMode::Udp,
                                "127.0.0.1",
                                peer.localPort(),
                                "127.0.0.1",
                                sessionPort});
        require(waitFor([&] { return networkOpen; }), "UDP source opens");
        require(session.loadSourceProtocolFile(sourceId, fromPath(protocolPath)),
                "source-specific protocol loads");
        require(configuredSource == sourceId && configuredMode == QStringLiteral("protocol"),
                "source-specific configuration is published to the UI model");
        require(!session.setSourceCsvFields(
                    sourceId,
                    {QStringLiteral("duplicate"), QStringLiteral("duplicate")}),
                "invalid source CSV fields are rejected transactionally");
        require(configuredMode == QStringLiteral("protocol"),
                "invalid source CSV fields preserve the active protocol");
        require(session.startSession(fromPath(root)), "session starts");
        require(!session.setSourceCsvFields(sourceId, {QStringLiteral("must_not_apply")}),
                "recording freezes source parser configuration");

        QByteArray frame;
        frame.append(static_cast<char>(0xAA));
        frame.append(static_cast<char>(42));
        require(peer.writeDatagram(frame, QHostAddress::LocalHost, sessionPort) == frame.size(),
                "binary UDP frame is sent");
        const auto field = sourceId.toStdString() + ".value";
        require(waitFor([&] {
                    const auto points = session.timeSeries().snapshot(field);
                    return points.size() == 1 && points.front().value == 42.0;
                }),
                "source-specific protocol produces its qualified field");
        require(session.timeSeries().snapshot(sourceId.toStdString() + ".default_value").empty(),
                "source override prevents the default CSV parser from seeing binary data");
        session.stopSession();
        session.disconnectNetwork();

        const auto sourceConfigurationPath = root / "configuration" / "source_0.json";
        const auto sourceProtocolPath = root / "protocol" / "source_0_initial.json";
        const auto originalSourceConfiguration = readFile(sourceConfigurationPath);
        const auto originalSourceProtocol = readFile(sourceProtocolPath);
        const auto sourceConfiguration = QJsonDocument::fromJson(originalSourceConfiguration);
        require(sourceConfiguration.isObject(), "recorded source configuration is valid JSON");
        const auto parser = sourceConfiguration.object()
                                .value(QStringLiteral("parser"))
                                .toObject();
        require(parser.value(QStringLiteral("format_version")).toInt() == 1 &&
                    parser.value(QStringLiteral("mode")).toString() ==
                        QStringLiteral("protocol") &&
                    parser.value(QStringLiteral("protocol"))
                            .toObject()
                            .value(QStringLiteral("snapshot"))
                            .toString() == QStringLiteral("protocol/source_0_initial.json"),
                "session freezes a versioned source parser profile");
        require(std::filesystem::exists(root / "protocol" / "source_0_initial.json"),
                "session stores the source-specific protocol snapshot");

        require(session.resetSourceParserConfiguration(sourceId),
                "live source override can be reset before replay");
        configuredMode.clear();
        require(session.openReplaySession(fromPath(root)),
                "session with a source-specific protocol reopens");
        require(configuredSource == sourceId && configuredMode == QStringLiteral("protocol"),
                "replay restores the source-specific parser profile");
        session.resumeReplay();
        require(waitFor([&] {
                    const auto points = session.timeSeries().snapshot(field);
                    return points.size() == 1 && points.front().value == 42.0;
                }),
                "replay routes raw bytes through the recorded source-specific protocol");
        require(session.timeSeries().snapshot(sourceId.toStdString() + ".default_value").empty(),
                "replay never sends source-specific binary data into the default CSV parser");
        session.closeReplay();

        auto damaged = sourceConfiguration.object();
        auto damagedParser = damaged.value(QStringLiteral("parser")).toObject();
        damagedParser.insert(QStringLiteral("format_version"), 999);
        damaged.insert(QStringLiteral("parser"), damagedParser);
        writeFile(sourceConfigurationPath, QJsonDocument(damaged).toJson());
        replayFailure.clear();
        require(!session.openReplaySession(fromPath(root)) && !replayFailure.isEmpty(),
                "future source parser configuration is rejected explicitly");
        networkOpen = false;
        session.connectNetwork({lab::adapters::network::NetworkMode::Udp,
                                "127.0.0.1",
                                peer.localPort(),
                                "127.0.0.1",
                                sessionPort});
        require(waitFor([&] { return networkOpen; }),
                "live UDP source reopens after rejected replay");
        require(peer.writeDatagram(frame, QHostAddress::LocalHost, sessionPort) ==
                    frame.size(),
                "binary UDP frame is resent after rejected replay");
        require(waitFor([&] {
                    const auto points = session.timeSeries().snapshot(field);
                    return points.size() == 1 && points.front().value == 42.0;
                }),
                "rejected replay preserves the active source-specific protocol");
        require(session.timeSeries()
                    .snapshot(sourceId.toStdString() + ".default_value")
                    .empty(),
                "rejected replay cannot replace the active source parser with CSV");
        session.disconnectNetwork();
        writeFile(sourceConfigurationPath, originalSourceConfiguration);
        require(QFile::remove(fromPath(sourceConfigurationPath)),
                "source parser configuration is temporarily removed");
        replayFailure.clear();
        require(!session.openReplaySession(fromPath(root)) && !replayFailure.isEmpty(),
                "declared source parser snapshot cannot silently go missing");
        writeFile(sourceConfigurationPath, originalSourceConfiguration);

        require(QFile::remove(fromPath(sourceProtocolPath)),
                "source protocol snapshot is temporarily removed");
        replayFailure.clear();
        require(!session.openReplaySession(fromPath(root)) && !replayFailure.isEmpty(),
                "declared source protocol snapshot cannot silently go missing");
        writeFile(sourceProtocolPath, originalSourceProtocol);

        auto mismatched = sourceConfiguration.object();
        mismatched.insert(QStringLiteral("id"), QStringLiteral("udp:other:1"));
        writeFile(sourceConfigurationPath, QJsonDocument(mismatched).toJson());
        replayFailure.clear();
        require(!session.openReplaySession(fromPath(root)) && !replayFailure.isEmpty(),
                "source parser identity mismatch is rejected explicitly");

        auto duplicateFields = sourceConfiguration.object();
        auto duplicateParser = duplicateFields.value(QStringLiteral("parser")).toObject();
        duplicateParser.insert(
            QStringLiteral("csv_fields"),
            QJsonArray{QStringLiteral("duplicate"), QStringLiteral("duplicate")});
        duplicateFields.insert(QStringLiteral("parser"), duplicateParser);
        writeFile(sourceConfigurationPath, QJsonDocument(duplicateFields).toJson());
        replayFailure.clear();
        require(!session.openReplaySession(fromPath(root)) && !replayFailure.isEmpty(),
                "duplicate per-source CSV fields are rejected explicitly");

        writeFile(sourceConfigurationPath, QByteArrayLiteral("{not-json"));
        replayFailure.clear();
        require(!session.openReplaySession(fromPath(root)) && !replayFailure.isEmpty(),
                "damaged source parser configuration is rejected explicitly");
        writeFile(sourceConfigurationPath, originalSourceConfiguration);

        std::filesystem::remove(protocolPath, cleanupError);
        std::filesystem::remove_all(root, cleanupError);
        std::cout << "Lab Debugger source parser session tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::filesystem::remove(protocolPath, cleanupError);
        std::filesystem::remove_all(root, cleanupError);
        std::cerr << "Lab Debugger source parser session tests failed: "
                  << exception.what() << '\n';
        return 1;
    }
}

#include "app/serial_session.hpp"

#include "lab/core/raw_log_reader.hpp"

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
#include <set>
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

QJsonObject readObject(const std::filesystem::path& path) {
    QFile file(fromPath(path));
    require(file.open(QIODevice::ReadOnly), "JSON file opens");
    const auto document = QJsonDocument::fromJson(file.readAll());
    require(document.isObject(), "JSON file contains an object");
    return document.object();
}

bool sourceOpen(const QVariantList& sources, const QString& sourceId) {
    for (const auto& value : sources) {
        const auto source = value.toMap();
        if (source.value(QStringLiteral("id")).toString() == sourceId) {
            return source.value(QStringLiteral("open")).toBool();
        }
    }
    return false;
}

QByteArray takeDatagram(QUdpSocket& socket) {
    if (!socket.hasPendingDatagrams()) return {};
    QByteArray result;
    result.resize(static_cast<qsizetype>(socket.pendingDatagramSize()));
    socket.readDatagram(result.data(), result.size());
    return result;
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    std::error_code cleanupError;
    const auto root = currentPath() /
                      ("lab-debugger-dynamic-sources-" +
                       QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString());
    try {
        QUdpSocket firstPeer;
        QUdpSocket secondPeer;
        require(firstPeer.bind(QHostAddress::LocalHost, 0), "first UDP peer binds");
        require(secondPeer.bind(QHostAddress::LocalHost, 0), "second UDP peer binds");
        const auto firstPort = reserveUdpPort();
        const auto secondPort = reserveUdpPort();
        const lab::adapters::network::NetworkSettings firstSettings{
            lab::adapters::network::NetworkMode::Udp,
            "127.0.0.1",
            firstPeer.localPort(),
            "127.0.0.1",
            firstPort};
        const lab::adapters::network::NetworkSettings secondSettings{
            lab::adapters::network::NetworkMode::Udp,
            "127.0.0.1",
            secondPeer.localPort(),
            "127.0.0.1",
            secondPort};
        const auto firstId = QString::fromStdString(
            lab::adapters::network::networkSourceId(firstSettings));
        const auto secondId = QString::fromStdString(
            lab::adapters::network::networkSourceId(secondSettings));

        lab::app::SerialSession session;
        QVariantList localSources;
        QString lastError;
        QObject::connect(&session,
                         &lab::app::SerialSession::localSourcesChanged,
                         &session,
                         [&](const QVariantList& sources) { localSources = sources; });
        QObject::connect(&session,
                         &lab::app::SerialSession::sourceError,
                         &session,
                         [&](const QString& error) { lastError = error; });

        session.connectNetwork(firstSettings);
        require(waitFor([&] { return sourceOpen(localSources, firstId); }),
                "first dynamic UDP source opens");
        session.connectNetwork(secondSettings);
        require(waitFor([&] {
                    return localSources.size() == 2 &&
                           sourceOpen(localSources, firstId) &&
                           sourceOpen(localSources, secondId);
                }),
                "second UDP source opens without closing the first");

        require(session.setSourceCsvFields(firstId, {QStringLiteral("left")}) &&
                    session.setSourceCsvFields(secondId, {QStringLiteral("right")}),
                "each dynamic source receives an independent parser configuration");

        session.setSendTargetSource(firstId);
        session.sendBytes(QByteArrayLiteral("first"));
        require(waitFor([&] { return firstPeer.hasPendingDatagrams(); }) &&
                    takeDatagram(firstPeer) == QByteArrayLiteral("first") &&
                    !secondPeer.hasPendingDatagrams(),
                "exact send target routes bytes only to the first UDP source");
        session.setSendTargetSource(secondId);
        session.sendBytes(QByteArrayLiteral("second"));
        require(waitFor([&] { return secondPeer.hasPendingDatagrams(); }) &&
                    takeDatagram(secondPeer) == QByteArrayLiteral("second"),
                "exact send target routes bytes to the second UDP source");

        require(session.startSession(fromPath(root)),
                "one Session freezes both open UDP sources");
        require(firstPeer.writeDatagram(
                    "1\n", QHostAddress::LocalHost, firstPort) == 2 &&
                    secondPeer.writeDatagram(
                        "2\n", QHostAddress::LocalHost, secondPort) == 2,
                "both UDP peers send interleaved samples");
        const auto firstField = firstId.toStdString() + ".left";
        const auto secondField = secondId.toStdString() + ".right";
        require(waitFor([&] {
                    const auto first = session.timeSeries().snapshot(firstField);
                    const auto second = session.timeSeries().snapshot(secondField);
                    return first.size() == 1 && first.front().value == 1.0 &&
                           second.size() == 1 && second.front().value == 2.0;
                }),
                "both source-specific fields reach independent curves");

        lastError.clear();
        require(!session.removeLocalSource(firstId) && !lastError.isEmpty(),
                "recording freezes the dynamic source set against removal");
        const lab::adapters::network::NetworkSettings undeclaredSettings{
            lab::adapters::network::NetworkMode::Udp,
            "127.0.0.1",
            firstPeer.localPort(),
            "127.0.0.1",
            reserveUdpPort()};
        session.connectNetwork(undeclaredSettings);
        require(localSources.size() == 2,
                "recording rejects an undeclared third local source");

        session.disconnectLocalSource(firstId);
        require(waitFor([&] { return !sourceOpen(localSources, firstId); }),
                "a declared source can disconnect during recording");
        session.reconnectLocalSource(firstId);
        require(waitFor([&] { return sourceOpen(localSources, firstId); }),
                "the same declared source can reconnect during recording");
        session.stopSession();

        const auto metadata = readObject(root / "metadata.json");
        require(metadata.value(QStringLiteral("software_version")).toString() ==
                    QStringLiteral("0.20.0"),
                "dynamic Session records the desktop 0.20 version");
        const auto sources = metadata.value(QStringLiteral("sources")).toArray();
        require(sources.size() == 2,
                "dynamic Session metadata contains both open sources");
        std::set<QString> metadataIds;
        for (qsizetype index = 0; index < sources.size(); ++index) {
            const auto source = sources[index].toObject();
            metadataIds.insert(source.value(QStringLiteral("id")).toString());
            const auto configuration = readObject(
                root / "configuration" /
                ("source_" + std::to_string(index) + ".json"));
            require(configuration.value(QStringLiteral("parser")).isObject(),
                    "every dynamic local source freezes its effective parser");
        }
        require(metadataIds == std::set<QString>{firstId, secondId},
                "metadata preserves both exact source identities");

        lab::core::RawLogReader raw;
        require(raw.open(root / "raw" / "stream.ldraw") &&
                    raw.recordCount() == 2,
                "dynamic Session records both RX payloads exactly once");
        std::set<std::string> rawIds;
        for (std::size_t index = 0; index < raw.recordCount(); ++index) {
            const auto chunk = raw.read(index);
            require(chunk.has_value(), "dynamic raw record reads");
            rawIds.insert(chunk->sourceId);
        }
        require(rawIds == std::set<std::string>{firstId.toStdString(),
                                                secondId.toStdString()},
                "raw log preserves both dynamic source identities");
        raw.close();

        require(session.removeLocalSource(firstId) &&
                    session.removeLocalSource(secondId) && localSources.isEmpty(),
                "stopped dynamic sources can be safely removed");

        std::filesystem::remove_all(root, cleanupError);
        require(!cleanupError, "dynamic source fixture is removed");
        std::cout << "Lab Debugger dynamic local source tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(root, cleanupError);
        std::cerr << "Lab Debugger dynamic local source tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}

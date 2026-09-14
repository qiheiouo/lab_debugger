#include "app/serial_session.hpp"

#include "lab/core/raw_log_reader.hpp"
#include "lab/core/timestamp.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QUdpSocket>
#include <QUuid>
#include <QVariantMap>

#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr qint64 manualOffsetNs = 50'000'000;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error("time-alignment session: " + message);
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

QByteArray readAll(const std::filesystem::path& path) {
    QFile file(fromPath(path));
    require(file.open(QIODevice::ReadOnly), "opens " + path.filename().string());
    const auto contents = file.readAll();
    require(file.error() == QFileDevice::NoError, "reads " + path.filename().string());
    return contents;
}

void writeAll(const std::filesystem::path& path, const QByteArray& contents) {
    QFile file(fromPath(path));
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
            "opens configuration fixture for writing");
    require(file.write(contents) == contents.size(), "writes configuration fixture");
    file.close();
}

QVariantList manualRule(const QString& sourceId, qint64 offsetNs) {
    QVariantMap rule;
    rule.insert(QStringLiteral("source_id"), sourceId);
    rule.insert(QStringLiteral("mode"), QStringLiteral("manual"));
    rule.insert(QStringLiteral("offset_ns"), offsetNs);
    return {rule};
}

void runTest() {
    QUdpSocket peer;
    require(peer.bind(QHostAddress::LocalHost, 0), "UDP fixture binds");
    const auto sessionPort = reserveUdpPort();
    const auto sourceId = QStringLiteral("udp:127.0.0.1:%1").arg(sessionPort);
    const auto field = sourceId.toStdString() + ".value";
    const auto root = currentPath() /
                      ("lab-debugger-time-alignment-" +
                       QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString());

    lab::app::SerialSession session;
    session.setCsvFields({QStringLiteral("value")});
    bool networkOpen = false;
    bool configurationRejected = false;
    QString replayFailure;
    QVariantList restoredRules;
    bool replayAtEnd = false;
    QObject::connect(&session,
                     &lab::app::SerialSession::networkStateChanged,
                     &session,
                     [&networkOpen](int state) {
                         networkOpen = state ==
                             static_cast<int>(lab::core::SourceState::Open);
                     });
    QObject::connect(&session,
                     &lab::app::SerialSession::timeAlignmentRulesConfigured,
                     &session,
                     [&configurationRejected](bool success, const QStringList&) {
                         configurationRejected = !success;
                     });
    QObject::connect(&session,
                     &lab::app::SerialSession::timeAlignmentRulesRestored,
                     &session,
                     [&restoredRules](const QVariantList& rules) {
                         restoredRules = rules;
                     });
    QObject::connect(&session,
                     &lab::app::SerialSession::replayOpenFailed,
                     &session,
                     [&replayFailure](const QString& message) {
                         replayFailure = message;
                     });
    QObject::connect(
        &session,
        &lab::app::SerialSession::replayStatusChanged,
        &session,
        [&replayAtEnd](bool, bool, bool atEnd, double, quint64, quint64,
                       qint64, qint64, qint64) { replayAtEnd = atEnd; });

    session.connectNetwork({lab::adapters::network::NetworkMode::Udp,
                            "127.0.0.1",
                            peer.localPort(),
                            "127.0.0.1",
                            sessionPort});
    require(waitFor([&] { return networkOpen; }), "UDP source opens");
    require(session.setTimeAlignmentRules(manualRule(sourceId, manualOffsetNs)),
            "manual alignment is accepted");
    require(session.startSession(fromPath(root)), "Session recording starts");

    const QByteArray row("42\n");
    require(peer.writeDatagram(row, QHostAddress::LocalHost, sessionPort) == row.size(),
            "UDP sample is sent");
    require(waitFor([&] { return session.timeSeries().snapshot(field).size() == 1; }),
            "aligned sample reaches the curve");
    const auto recordedPoint = session.timeSeries().snapshot(field).front();

    configurationRejected = false;
    QVariantMap receiveRule;
    receiveRule.insert(QStringLiteral("source_id"), sourceId);
    receiveRule.insert(QStringLiteral("mode"), QStringLiteral("receive"));
    require(!session.setTimeAlignmentRules({receiveRule}) && configurationRejected,
            "alignment cannot change while Session sources are frozen");
    session.stopSession();

    lab::core::RawLogReader reader;
    require(reader.open(root / "raw" / "stream.ldraw") && reader.recordCount() == 1,
            "recorded raw log contains one source sample");
    const auto raw = reader.read(0);
    require(raw && raw->sourceTimestamp > 0 && raw->receiveTimestamp > 0,
            "raw record keeps both timestamp domains");
    require(recordedPoint.timestamp == raw->sourceTimestamp + manualOffsetNs,
            "curve uses source timestamp plus configured offset");
    reader.close();

    const auto values = readAll(root / "values.csv");
    const auto lines = values.trimmed().split('\n');
    require(lines.size() == 2 &&
                lines.front().trimmed() ==
                    "timestamp_ns,source_id,sequence,field,value,unit,"
                    "source_timestamp_ns,receive_timestamp_ns",
            "values.csv declares aligned and original timestamp columns");
    const auto columns = lines.back().trimmed().split(',');
    require(columns.size() == 8 && columns[0].toLongLong() == recordedPoint.timestamp &&
                columns[6].toLongLong() == raw->sourceTimestamp &&
                columns[7].toLongLong() == raw->receiveTimestamp,
            "values.csv preserves evidence for the effective timeline");

    const auto alignmentDocument = QJsonDocument::fromJson(
        readAll(root / "configuration" / "time_alignment.json"));
    const auto alignmentObject = alignmentDocument.object();
    const auto persistedRules = alignmentObject.value(QStringLiteral("rules")).toArray();
    require(alignmentDocument.isObject() &&
                alignmentObject.value(QStringLiteral("format_version")).toInt() == 1 &&
                persistedRules.size() == 1 &&
                persistedRules.at(0).toObject()
                        .value(QStringLiteral("mode")).toString() ==
                    QStringLiteral("manual") &&
                persistedRules.at(0).toObject()
                        .value(QStringLiteral("offset_ns")).toInteger() == manualOffsetNs,
            "Session stores a versioned deterministic alignment snapshot");
    const auto metadata = QJsonDocument::fromJson(readAll(root / "metadata.json")).object();
    require(metadata.value(QStringLiteral("time_alignment")).toString() ==
                QStringLiteral("configuration/time_alignment.json"),
            "metadata advertises the alignment snapshot");

    require(session.openReplaySession(fromPath(root)), "recorded Session reopens");
    require(restoredRules.size() == 1 &&
                restoredRules.front().toMap()
                        .value(QStringLiteral("offset_ns")).toLongLong() == manualOffsetNs,
            "replay restores the frozen alignment rule");
    replayAtEnd = false;
    session.setReplaySpeed(10.0);
    session.resumeReplay();
    require(waitFor([&] { return replayAtEnd; }), "aligned replay reaches the end");
    require(waitFor([&] { return session.timeSeries().snapshot(field).size() == 1; }),
            "aligned replay recreates the curve");
    require(session.timeSeries().snapshot(field).front().timestamp ==
                recordedPoint.timestamp,
            "replay deterministically recreates the aligned timestamp");
    session.closeReplay();

    require(session.setTimeAlignmentRules(manualRule(sourceId, 75'000'000)),
            "a valid live rule is active before unsafe replay input");
    writeAll(root / "configuration" / "time_alignment.json",
             "{\"format_version\":999,\"rules\":[]}");
    replayFailure.clear();
    require(!session.openReplaySession(fromPath(root)) && !replayFailure.isEmpty(),
            "future alignment configuration is rejected explicitly");

    networkOpen = false;
    session.connectNetwork({lab::adapters::network::NetworkMode::Udp,
                            "127.0.0.1",
                            peer.localPort(),
                            "127.0.0.1",
                            sessionPort});
    require(waitFor([&] { return networkOpen; }),
            "UDP source reopens after rejected replay input");
    const auto beforeSend = lab::core::nowTimestampNs();
    require(peer.writeDatagram(row, QHostAddress::LocalHost, sessionPort) == row.size(),
            "UDP sample is resent after rejected replay input");
    require(waitFor([&] { return session.timeSeries().snapshot(field).size() == 1; }),
            "post-rejection sample reaches the curve");
    require(session.timeSeries().snapshot(field).front().timestamp >=
                beforeSend + 70'000'000,
            "rejected replay configuration does not replace the active rule");
    session.disconnectNetwork();

    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    require(!cleanupError, "removes time-alignment fixture");
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    try {
        runTest();
        std::cout << "Lab Debugger time-alignment Session tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Lab Debugger time-alignment Session tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}

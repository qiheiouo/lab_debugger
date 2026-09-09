#include "app/serial_session.hpp"
#include "lab/core/raw_log_writer.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QUuid>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
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

void createSession(const std::filesystem::path& directory, bool rawOnly) {
    std::filesystem::create_directories(directory / "raw");
    std::filesystem::create_directories(directory / "configuration");
    std::filesystem::create_directories(directory / "protocol");
    lab::core::RawLogWriter writer;
    require(writer.open(directory / "raw" / "stream.ldraw"), "raw fixture opens");
    lab::core::DataChunk chunk;
    chunk.sourceId = "routing-test";
    chunk.sourceTimestamp = 100;
    chunk.receiveTimestamp = 100;
    chunk.sequence = 1;
    chunk.payload = {'4', '2', '\n'};
    require(writer.write(chunk), "first raw fixture source is written");
    chunk.sourceId = "routing-other";
    chunk.sourceTimestamp = 200;
    chunk.receiveTimestamp = 200;
    chunk.payload = {'7', '\n'};
    require(writer.write(chunk) && writer.close(), "multi-source raw fixture is written");

    QFile fields(fromPath(directory / "configuration" / "csv_fields.txt"));
    require(fields.open(QIODevice::WriteOnly | QIODevice::Truncate),
            "CSV field fixture opens");
    require(fields.write("value\n") == 6, "CSV field fixture is written");
    fields.close();

    QJsonObject metadata;
    metadata.insert(QStringLiteral("format"), QStringLiteral("lab-debug-session"));
    metadata.insert(QStringLiteral("format_version"), 1);
    if (rawOnly) {
        metadata.insert(QStringLiteral("replay_mode"), QStringLiteral("raw-only"));
    }
    QFile output(fromPath(directory / "metadata.json"));
    require(output.open(QIODevice::WriteOnly | QIODevice::Truncate),
            "metadata fixture opens");
    require(output.write(QJsonDocument(metadata).toJson()) > 0,
            "metadata fixture is written");
}

void createStructuredRosbagSession(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory / "raw");
    std::filesystem::create_directories(directory / "configuration");
    std::filesystem::create_directories(directory / "protocol");
    lab::core::RawLogWriter writer;
    require(writer.open(directory / "raw" / "stream.ldraw"),
            "structured rosbag raw fixture opens");
    lab::core::DataChunk chunk;
    chunk.sourceId = lab::adapters::rosbag2::rosbag2SourceId(
        "/temperature", "std_msgs/msg/Float64");
    chunk.sourceTimestamp = 100;
    chunk.receiveTimestamp = 100;
    chunk.sequence = 0;
    chunk.payload = {0x00, 0x01, 0x00, 0x00,
                     0x00, 0x00, 0x00, 0x00,
                     0x00, 0x40, 0x45, 0x40};
    require(writer.write(chunk) && writer.close(),
            "structured rosbag raw fixture is written");

    QJsonObject topic;
    topic.insert(QStringLiteral("name"), QStringLiteral("/temperature"));
    topic.insert(QStringLiteral("type"), QStringLiteral("std_msgs/msg/Float64"));
    topic.insert(QStringLiteral("serialization_format"), QStringLiteral("cdr"));
    topic.insert(QStringLiteral("field_mapping"), QStringLiteral("built-in"));
    QJsonObject catalog;
    catalog.insert(QStringLiteral("topics"), QJsonArray{topic});
    QFile catalogOutput(
        fromPath(directory / "configuration" / "rosbag2.json"));
    require(catalogOutput.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                catalogOutput.write(QJsonDocument(catalog).toJson()) > 0,
            "structured rosbag catalog is written");

    QJsonObject metadata;
    metadata.insert(QStringLiteral("format"), QStringLiteral("lab-debug-session"));
    metadata.insert(QStringLiteral("format_version"), 1);
    metadata.insert(QStringLiteral("replay_mode"),
                    QStringLiteral("rosbag2-structured"));
    QFile metadataOutput(fromPath(directory / "metadata.json"));
    require(metadataOutput.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                metadataOutput.write(QJsonDocument(metadata).toJson()) > 0,
            "structured rosbag metadata is written");
}

bool waitForReplayEnd(lab::app::SerialSession& session) {
    bool atEnd = false;
    const auto connection = QObject::connect(
        &session,
        &lab::app::SerialSession::replayStatusChanged,
        &session,
        [&atEnd](bool, bool, bool end, double, quint64, quint64,
                 qint64, qint64, qint64) { atEnd = end; });
    session.setReplaySpeed(10.0);
    session.resumeReplay();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!atEnd && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    for (int attempt = 0; attempt < 20; ++attempt) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    QObject::disconnect(connection);
    return atEnd;
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    try {
        const auto root = toPath(QDir::currentPath()) /
                          ("lab-debugger-routing-test-" +
                           QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString());
        const auto normal = root / "normal";
        const auto rawOnly = root / "raw_only";
        const auto structuredRosbag = root / "structured_rosbag";
        createSession(normal, false);
        createSession(rawOnly, true);
        createStructuredRosbagSession(structuredRosbag);

        bool openedRawOnly = false;
        bool openedStructuredRosbag = false;
        QStringList replayFields;
        lab::app::SerialSession session;
        QObject::connect(&session,
                         &lab::app::SerialSession::replayOpened,
                         &session,
                         [&openedRawOnly, &openedStructuredRosbag](
                             const QString&, bool, bool raw, bool structured) {
                             openedRawOnly = raw;
                             openedStructuredRosbag = structured;
                         });
        QObject::connect(&session,
                         &lab::app::SerialSession::replayFieldsDiscovered,
                         &session,
                         [&replayFields](const QStringList& fields) {
                             replayFields = fields;
                         });

        require(session.openReplaySession(fromPath(normal)), "normal Session opens");
        require(!openedRawOnly, "normal Session is not marked raw-only");
        require(waitForReplayEnd(session), "normal replay reaches its end");
        const auto firstReplaySeries =
            session.timeSeries().snapshot("routing-test.value");
        const auto secondReplaySeries =
            session.timeSeries().snapshot("routing-other.value");
        require(firstReplaySeries.size() == 1 && secondReplaySeries.size() == 1 &&
                    firstReplaySeries.front().value == 42.0 &&
                    secondReplaySeries.front().value == 7.0,
                "normal replay keeps same-named fields from different sources separate");

        require(session.openReplaySession(fromPath(rawOnly)), "raw-only Session opens");
        require(openedRawOnly, "raw-only metadata reaches the application layer");
        require(waitForReplayEnd(session), "raw-only replay reaches its end");
        require(session.timeSeries().fields().empty(),
                "raw-only replay never sends CDR bytes into CSV processing");

        require(session.openReplaySession(fromPath(structuredRosbag)),
                "structured rosbag Session opens");
        require(!openedRawOnly && openedStructuredRosbag,
                "structured rosbag replay mode reaches the application layer");
        require(waitForReplayEnd(session),
                "structured rosbag replay reaches its end");
        const auto points = session.timeSeries().snapshot("/temperature.data");
        require(points.size() == 1 && points.front().timestamp == 100 &&
                    points.front().value == 42.5,
                "structured rosbag CDR enters the curve without entering CSV parsing");
        require(replayFields.contains(QStringLiteral("/temperature.data")),
                "structured replay publishes discovered curve fields to the UI");
        session.closeReplay();

        QFile invalidMetadata(fromPath(structuredRosbag / "metadata.json"));
        require(invalidMetadata.open(QIODevice::ReadOnly),
                "structured metadata reopens for validation");
        auto invalidDocument = QJsonDocument::fromJson(invalidMetadata.readAll());
        invalidMetadata.close();
        auto invalidObject = invalidDocument.object();
        invalidObject.insert(QStringLiteral("replay_mode"), 7);
        require(invalidMetadata.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                    invalidMetadata.write(QJsonDocument(invalidObject).toJson()) > 0,
                "invalid replay route fixture is written");
        invalidMetadata.close();
        require(!session.openReplaySession(fromPath(structuredRosbag)),
                "invalid replay route cannot send CDR into the default parser");

        invalidObject.insert(QStringLiteral("replay_mode"),
                             QStringLiteral("unknown-cdr-route"));
        require(invalidMetadata.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                    invalidMetadata.write(QJsonDocument(invalidObject).toJson()) > 0,
                "unknown replay route fixture is written");
        invalidMetadata.close();
        require(!session.openReplaySession(fromPath(structuredRosbag)),
                "unknown replay route cannot send CDR into the default parser");

        require(invalidMetadata.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                    invalidMetadata.write("{not-json") == 9,
                "damaged metadata fixture is written");
        invalidMetadata.close();
        require(!session.openReplaySession(fromPath(structuredRosbag)),
                "damaged metadata cannot send CDR into the default parser");

        const QByteArray oversizedMetadata(4 * 1024 * 1024 + 1, 'x');
        require(invalidMetadata.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                    invalidMetadata.write(oversizedMetadata) == oversizedMetadata.size(),
                "oversized metadata fixture is written");
        invalidMetadata.close();
        require(!session.openReplaySession(fromPath(structuredRosbag)),
                "oversized metadata cannot send CDR into the default parser");

        std::error_code cleanupError;
        std::filesystem::remove_all(root, cleanupError);
        require(!cleanupError, "routing fixture is removed");
        std::cout << "Lab Debugger replay routing tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Lab Debugger replay routing tests failed: " << error.what() << '\n';
        return 1;
    }
}

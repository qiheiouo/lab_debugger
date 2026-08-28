#include "app/serial_session.hpp"
#include "lab/core/raw_log_writer.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
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
    require(writer.write(chunk) && writer.close(), "raw fixture is written");

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
        createSession(normal, false);
        createSession(rawOnly, true);

        bool openedRawOnly = false;
        lab::app::SerialSession session;
        QObject::connect(&session,
                         &lab::app::SerialSession::replayOpened,
                         &session,
                         [&openedRawOnly](const QString&, bool, bool value) {
                             openedRawOnly = value;
                         });

        require(session.openReplaySession(fromPath(normal)), "normal Session opens");
        require(!openedRawOnly, "normal Session is not marked raw-only");
        require(waitForReplayEnd(session), "normal replay reaches its end");
        require(session.timeSeries().snapshot("value").size() == 1,
                "normal replay routes CSV text into the processing pipeline");

        require(session.openReplaySession(fromPath(rawOnly)), "raw-only Session opens");
        require(openedRawOnly, "raw-only metadata reaches the application layer");
        require(waitForReplayEnd(session), "raw-only replay reaches its end");
        require(session.timeSeries().fields().empty(),
                "raw-only replay never sends CDR bytes into CSV processing");
        session.closeReplay();

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

#include "lab/adapters/rosbag2/rosbag2_importer.hpp"
#include "lab/core/raw_log_reader.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::filesystem::path toPath(const QString& path) {
#ifdef _WIN32
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toStdString());
#endif
}

struct Message {
    int topicId{};
    std::int64_t timestamp{};
    QByteArray payload;
};

void createBag(const QString& path,
               const QString& format,
               const std::vector<Message>& messages,
               bool secondTopic = false) {
    const auto connection = QStringLiteral("rosbag2-test-%1")
                                .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(path);
        require(database.open(), database.lastError().text().toStdString());
        QSqlQuery query(database);
        require(query.exec(QStringLiteral(
                    "CREATE TABLE topics (id INTEGER PRIMARY KEY, name TEXT NOT NULL, "
                    "type TEXT NOT NULL, serialization_format TEXT NOT NULL)")),
                query.lastError().text().toStdString());
        require(query.exec(QStringLiteral(
                    "CREATE TABLE messages (id INTEGER PRIMARY KEY, topic_id INTEGER NOT NULL, "
                    "timestamp INTEGER NOT NULL, data BLOB NOT NULL)")),
                query.lastError().text().toStdString());
        query.prepare(QStringLiteral(
            "INSERT INTO topics(id, name, type, serialization_format) VALUES(?, ?, ?, ?)"));
        query.addBindValue(1);
        query.addBindValue(QStringLiteral("/temperature"));
        query.addBindValue(QStringLiteral("std_msgs/msg/Float64"));
        query.addBindValue(format);
        require(query.exec(), query.lastError().text().toStdString());
        if (secondTopic) {
            query.prepare(QStringLiteral(
                "INSERT INTO topics(id, name, type, serialization_format) VALUES(?, ?, ?, ?)"));
            query.addBindValue(2);
            query.addBindValue(QStringLiteral("/status"));
            query.addBindValue(QStringLiteral("std_msgs/msg/String"));
            query.addBindValue(format);
            require(query.exec(), query.lastError().text().toStdString());
        }
        std::int64_t row = 1;
        for (const auto& message : messages) {
            query.prepare(QStringLiteral(
                "INSERT INTO messages(id, topic_id, timestamp, data) VALUES(?, ?, ?, ?)"));
            query.addBindValue(row++);
            query.addBindValue(message.topicId);
            query.addBindValue(message.timestamp);
            query.addBindValue(message.payload);
            require(query.exec(), query.lastError().text().toStdString());
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
}

void testSplitBagImport(const std::filesystem::path& root) {
    const auto source = root / "split_bag";
    std::filesystem::create_directories(source);
    createBag(QString::fromStdWString((source / "bag_0.db3").wstring()),
              QStringLiteral("cdr"),
              {{1, 100, QByteArray::fromHex("01")},
               {1, 300, QByteArray::fromHex("03")}},
              true);
    createBag(QString::fromStdWString((source / "bag_1.db3").wstring()),
              QStringLiteral("cdr"),
              {{2, 200, QByteArray::fromHex("02")},
               {1, 300, QByteArray::fromHex("04")}},
              true);

    const auto destination = root / "imported_session";
    std::uint64_t lastProgress{};
    const auto result = lab::adapters::rosbag2::importRosbag2(
        {source, destination, "split-import", "0.11.0"},
        [&lastProgress](std::uint64_t imported, std::uint64_t) {
            lastProgress = imported;
            return true;
        });
    require(result.success, "split rosbag2 import succeeds: " + result.error);
    require(result.databaseCount == 2, "split database count is preserved");
    require(result.messageCount == 4 && result.payloadBytes == 4,
            "message and byte counts are exact");
    require(result.topics.size() == 2, "topic catalog is merged across split files");
    require(result.firstTimestamp == 100 && result.lastTimestamp == 300,
            "imported timeline covers all databases");
    require(lastProgress == 4, "progress reaches the complete message count");

    lab::core::RawLogReader reader;
    require(reader.open(destination / "raw" / "stream.ldraw"),
            "imported raw log opens");
    require(reader.recordCount() == 4, "imported raw log has every message");
    const std::vector<std::uint8_t> expected{1, 2, 3, 4};
    const std::vector<lab::core::Timestamp> timestamps{100, 200, 300, 300};
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto chunk = reader.read(index);
        require(chunk.has_value(), "imported raw record is readable");
        require(chunk->sourceTimestamp == timestamps[index] &&
                    chunk->receiveTimestamp == timestamps[index],
                "bag timestamp is preserved as both Session timestamps");
        require(chunk->sequence == index, "import assigns stable global sequence numbers");
        require(chunk->payload == std::vector<std::uint8_t>{expected[index]},
                "split records use deterministic timestamp ordering");
        require(chunk->sourceId.starts_with("rosbag2:/"),
                "raw records identify their rosbag2 topic");
    }

    QFile metadata(QString::fromStdWString((destination / "metadata.json").wstring()));
    require(metadata.open(QIODevice::ReadOnly), "imported metadata exists");
    const auto document = QJsonDocument::fromJson(metadata.readAll());
    require(document.isObject(), "imported metadata is valid JSON");
    require(document.object().value(QStringLiteral("replay_mode")).toString() ==
                QStringLiteral("raw-only"),
            "imported Session prevents CDR from entering CSV parsing");
    require(std::filesystem::exists(destination / "configuration" / "rosbag2.json"),
            "imported topic catalog is retained");
}

void testCancellationAndValidation(const std::filesystem::path& root) {
    const auto bag = root / "single.db3";
    createBag(QString::fromStdWString(bag.wstring()),
              QStringLiteral("cdr"),
              {{1, 100, QByteArray::fromHex("0102")}});
    const auto cancelledDestination = root / "cancelled_session";
    const auto cancelled = lab::adapters::rosbag2::importRosbag2(
        {bag, cancelledDestination, "cancelled", "0.11.0"},
        [](std::uint64_t, std::uint64_t) { return false; });
    require(!cancelled.success && cancelled.cancelled,
            "cancel callback stops import explicitly");
    require(!std::filesystem::exists(cancelledDestination),
            "cancelled import does not publish a partial Session");

    const auto unsupportedBag = root / "unsupported.db3";
    createBag(QString::fromStdWString(unsupportedBag.wstring()),
              QStringLiteral("json"),
              {{1, 100, QByteArrayLiteral("{}")}});
    const auto unsupportedDestination = root / "unsupported_session";
    const auto unsupported = lab::adapters::rosbag2::importRosbag2(
        {unsupportedBag, unsupportedDestination, "unsupported", "0.11.0"});
    require(!unsupported.success &&
                unsupported.error.find("serialization format") != std::string::npos,
            "non-CDR bag fails with a clear format error");
    require(!std::filesystem::exists(unsupportedDestination),
            "invalid input does not publish a partial Session");

    const auto occupied = root / "occupied_session";
    std::filesystem::create_directories(occupied);
    const auto existing = lab::adapters::rosbag2::importRosbag2(
        {bag, occupied, "occupied", "0.11.0"});
    require(!existing.success &&
                existing.error.find("already exists") != std::string::npos,
            "import never overwrites an existing destination");
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Lab Debugger rosbag2 tests"));
    try {
        const auto root = toPath(QDir::currentPath()) /
                          ("lab-debugger-rosbag2-test-" +
                           QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString());
        std::filesystem::create_directories(root);
        testSplitBagImport(root);
        testCancellationAndValidation(root);
        std::error_code cleanupError;
        std::filesystem::remove_all(root, cleanupError);
        require(!cleanupError, "temporary test directory is removed");
        std::cout << "Lab Debugger rosbag2 tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Lab Debugger rosbag2 tests failed: " << error.what() << '\n';
        return 1;
    }
}

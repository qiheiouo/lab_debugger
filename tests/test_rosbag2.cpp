#include "lab/adapters/rosbag2/rosbag2_importer.hpp"
#include "lab/core/raw_log_reader.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include <algorithm>
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
            query.addBindValue(static_cast<qlonglong>(row++));
            query.addBindValue(message.topicId);
            query.addBindValue(static_cast<qlonglong>(message.timestamp));
            query.addBindValue(message.payload);
            require(query.exec(), query.lastError().text().toStdString());
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
}

void writeFile(const std::filesystem::path& path, const QByteArray& bytes) {
    QFile file(QString::fromStdWString(path.wstring()));
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
            "test fixture file opens");
    require(file.write(bytes) == bytes.size(), "test fixture file is complete");
}

QByteArray readFile(const std::filesystem::path& path) {
    QFile file(QString::fromStdWString(path.wstring()));
    require(file.open(QIODevice::ReadOnly), "test result file opens");
    return file.readAll();
}

std::vector<std::pair<std::int64_t, QByteArray>> readDatabaseMessages(
    const std::filesystem::path& path) {
    const auto connection = QStringLiteral("rosbag2-external-%1")
                                .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    std::vector<std::pair<std::int64_t, QByteArray>> messages;
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        database.setDatabaseName(QString::fromStdWString(path.wstring()));
        require(database.open(), database.lastError().text().toStdString());
        QSqlQuery query(database);
        require(query.exec(QStringLiteral(
                    "SELECT timestamp, data FROM messages ORDER BY timestamp, id")),
                query.lastError().text().toStdString());
        while (query.next()) {
            bool valid = false;
            const auto timestamp = query.value(0).toLongLong(&valid);
            require(valid, "external bag message timestamp is valid");
            messages.emplace_back(timestamp, query.value(1).toByteArray());
        }
        require(!query.lastError().isValid(), "external bag messages are readable");
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return messages;
}

void verifyExternalBag(const std::filesystem::path& source,
                       const std::filesystem::path& destination) {
    const auto inspection = lab::adapters::rosbag2::inspectRosbag2(source);
    require(inspection.success, "external bag inspection succeeds: " + inspection.error);
    require(inspection.databaseCount == 1,
            "external validation bag contains exactly one SQLite database");
    const auto topic = std::find_if(
        inspection.topics.begin(), inspection.topics.end(), [](const auto& item) {
            return item.name == "/lab_debugger_validation" &&
                   item.type == "std_msgs/msg/Float64" &&
                   item.serializationFormat == "cdr";
        });
    require(topic != inspection.topics.end() && topic->messageCount > 1,
            "external bag contains multiple Float64 messages");
    require(inspection.metadata.present && inspection.metadata.parsed &&
                inspection.metadata.storageIdentifier == "sqlite3" &&
                inspection.metadata.storageIdentifierMatches == true &&
                inspection.metadata.databaseFilesMatch == true &&
                inspection.metadata.messageCountMatches == true,
            "external metadata stable fields match SQLite truth");

    std::filesystem::path databasePath;
    for (const auto& entry : std::filesystem::directory_iterator(source)) {
        if (entry.is_regular_file() && entry.path().extension() == ".db3") {
            require(databasePath.empty(),
                    "external validation bag has only one .db3 file");
            databasePath = entry.path();
        }
    }
    require(!databasePath.empty(), "external validation bag .db3 file exists");
    const auto databaseMessages = readDatabaseMessages(databasePath);
    require(databaseMessages.size() == inspection.messageCount,
            "external SQLite message count matches importer inspection");

    const auto result = lab::adapters::rosbag2::importRosbag2(
        {source, destination, "real-humble-bag", "0.14.0", {}});
    require(result.success, "external bag import succeeds: " + result.error);
    require(result.messageCount == databaseMessages.size() &&
                result.mappedMessageCount == result.messageCount &&
                result.sampleCount == result.messageCount &&
                result.mappingFailures == 0,
            "external Float64 messages all produce structured curves");

    const auto sourceMetadata = readFile(source / "metadata.yaml");
    const auto savedMetadata = readFile(
        destination / "configuration" / "rosbag2_metadata.yaml");
    const auto digest = QCryptographicHash::hash(sourceMetadata,
                                                 QCryptographicHash::Sha256)
                            .toHex()
                            .toStdString();
    require(savedMetadata == sourceMetadata && result.metadata.sha256 == digest,
            "external metadata bytes and SHA-256 are preserved exactly");

    lab::core::RawLogReader reader;
    require(reader.open(destination / "raw" / "stream.ldraw") &&
                reader.recordCount() == databaseMessages.size(),
            "external Session raw log contains every SQLite message");
    for (std::size_t index = 0; index < databaseMessages.size(); ++index) {
        const auto chunk = reader.read(index);
        const auto& [timestamp, payload] = databaseMessages[index];
        const auto* begin = reinterpret_cast<const std::uint8_t*>(payload.constData());
        const std::vector<std::uint8_t> expected(begin, begin + payload.size());
        require(chunk && chunk->sourceTimestamp == timestamp &&
                    chunk->payload == expected,
                "external raw CDR remains byte-for-byte identical");
    }
    const auto values = readFile(destination / "values.csv");
    require(values.contains("/lab_debugger_validation.data,42.5,"),
            "external Float64 CDR produces the expected structured value");

    std::cout << "External Humble bag verified: databases="
              << result.databaseCount << " messages=" << result.messageCount
              << " version=" << inspection.metadata.version.value_or(0)
              << " duration_ns="
              << inspection.metadata.durationNanoseconds.value_or(0)
              << " starting_time_ns="
              << inspection.metadata.startingTimeNanoseconds.value_or(0)
              << " compression=" << inspection.metadata.compressionMode << '/'
              << inspection.metadata.compressionFormat
              << " ros_distro=" << inspection.metadata.rosDistro
              << " sha256=" << result.metadata.sha256 << '\n';
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

    const auto inspection = lab::adapters::rosbag2::inspectRosbag2(source);
    require(inspection.success, "split rosbag2 inspection succeeds: " + inspection.error);
    require(inspection.databaseCount == 2 && inspection.topics.size() == 2,
            "inspection merges topic catalogs across split files");
    require(inspection.messageCount == 4 && inspection.payloadBytes == 4,
            "inspection reports exact total counts without importing");
    require(inspection.topics[0].structuredFields != inspection.topics[1].structuredFields,
            "inspection distinguishes curve-capable and raw-only message types");

    const auto destination = root / "imported_session";
    std::uint64_t lastProgress{};
    const auto result = lab::adapters::rosbag2::importRosbag2(
        {source, destination, "split-import", "0.14.0", {}},
        [&lastProgress](std::uint64_t imported, std::uint64_t) {
            lastProgress = imported;
            return true;
        });
    require(result.success, "split rosbag2 import succeeds: " + result.error);
    require(result.databaseCount == 2, "split database count is preserved");
    require(result.messageCount == 4 && result.payloadBytes == 4,
            "message and byte counts are exact");
    require(result.sampleCount == 0 && result.mappingFailures == 3,
            "damaged built-in CDR remains raw-only and is counted without aborting import");
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

    const auto filteredDestination = root / "filtered_session";
    lab::adapters::rosbag2::Rosbag2ImportOptions filteredOptions{
        source, filteredDestination, "filtered-import", "0.14.0", {}};
    filteredOptions.includedTopics.push_back(
        {"/status", "std_msgs/msg/String"});
    const auto filtered = lab::adapters::rosbag2::importRosbag2(filteredOptions);
    require(filtered.success, "selected Topic import succeeds: " + filtered.error);
    require(filtered.topics.size() == 1 && filtered.messageCount == 1 &&
                filtered.payloadBytes == 1,
            "selected Topic controls catalog and aggregate counts");
    lab::core::RawLogReader filteredReader;
    require(filteredReader.open(filteredDestination / "raw" / "stream.ldraw") &&
                filteredReader.recordCount() == 1,
            "selected Topic produces only its raw records");
    const auto selectedChunk = filteredReader.read(0);
    require(selectedChunk && selectedChunk->sourceTimestamp == 200 &&
                selectedChunk->payload == std::vector<std::uint8_t>{2} &&
                selectedChunk->sourceId.find("/status") != std::string::npos,
            "selected Topic preserves its timestamp, payload, and source id");
    QFile filteredCatalog(
        QString::fromStdWString((filteredDestination / "configuration" / "rosbag2.json")
                                    .wstring()));
    require(filteredCatalog.open(QIODevice::ReadOnly),
            "selected Topic catalog exists");
    const auto filteredDocument = QJsonDocument::fromJson(filteredCatalog.readAll());
    require(filteredDocument.object()
                    .value(QStringLiteral("selection_mode"))
                    .toString() == QStringLiteral("explicit") &&
                filteredDocument.object()
                        .value(QStringLiteral("topics"))
                        .toArray()
                        .size() == 1,
            "Session configuration records the explicit filtered catalog");
}

void testStructuredImport(const std::filesystem::path& root) {
    const auto bag = root / "structured.db3";
    createBag(QString::fromStdWString(bag.wstring()),
              QStringLiteral("cdr"),
              {{1,
                123,
                QByteArray::fromHex("000100000000000000404540")}});
    const auto destination = root / "structured_session";
    const auto result = lab::adapters::rosbag2::importRosbag2(
        {bag, destination, "structured", "0.14.0", {}});
    require(result.success && result.messageCount == 1 &&
                result.mappedMessageCount == 1 && result.sampleCount == 1 &&
                result.mappingFailures == 0,
            "valid common CDR produces one structured numeric sample");

    QFile values(QString::fromStdWString((destination / "values.csv").wstring()));
    require(values.open(QIODevice::ReadOnly | QIODevice::Text),
            "structured values.csv exists");
    const auto valuesText = values.readAll();
    require(valuesText.contains("123,rosbag2:/temperature [std_msgs/msg/Float64],0,/temperature.data,42.5,"),
            "values.csv preserves bag time, source, field path, value, and sequence");

    QFile metadata(QString::fromStdWString((destination / "metadata.json").wstring()));
    require(metadata.open(QIODevice::ReadOnly), "structured metadata exists");
    const auto document = QJsonDocument::fromJson(metadata.readAll()).object();
    require(document.value(QStringLiteral("replay_mode")).toString() ==
                QStringLiteral("rosbag2-structured") &&
                document.value(QStringLiteral("counts"))
                        .toObject()
                        .value(QStringLiteral("samples"))
                        .toInteger() == 1 &&
                document.value(QStringLiteral("import"))
                        .toObject()
                        .value(QStringLiteral("mapped_message_count"))
                        .toInteger() == 1,
            "metadata enables safe structured rosbag replay and records exact counts");

    QFile catalog(
        QString::fromStdWString((destination / "configuration" / "rosbag2.json").wstring()));
    require(catalog.open(QIODevice::ReadOnly), "structured Topic catalog exists");
    const auto topics = QJsonDocument::fromJson(catalog.readAll())
                            .object()
                            .value(QStringLiteral("topics"))
                            .toArray();
    require(topics.size() == 1 &&
                topics.at(0)
                        .toObject()
                        .value(QStringLiteral("field_mapping"))
                        .toString() == QStringLiteral("built-in"),
            "Topic catalog records built-in structured replay capability");
}

void testMetadataPreservationAndValidation(const std::filesystem::path& root) {
    const auto source = root / "metadata_bag";
    std::filesystem::create_directories(source);
    createBag(QString::fromStdWString((source / "bag_0.db3").wstring()),
              QStringLiteral("cdr"),
              {{1, 100, QByteArray::fromHex("01")},
               {1, 200, QByteArray::fromHex("02")},
               {2, 300, QByteArray::fromHex("03")}},
              true);
    const QByteArray metadata = QByteArrayLiteral(
        "rosbag2_bagfile_information:\n"
        "  version: 5\n"
        "  storage_identifier: sqlite3\n"
        "  duration:\n"
        "    nanoseconds: 100\n"
        "  starting_time:\n"
        "    nanoseconds_since_epoch: 100\n"
        "  message_count: 3\n"
        "  relative_file_paths:\n"
        "    - \"bag_0.db3\"\n"
        "  compression_format: ''\n"
        "  compression_mode: ''\n"
        "  ros_distro: humble\n"
        "  topics_with_message_count:\n"
        "    - topic_metadata:\n"
        "        name: /temperature\n"
        "        type: std_msgs/msg/Float64\n"
        "        serialization_format: cdr\n"
        "        offered_qos_profiles: |-\n"
        "          - history: 3\n"
        "            depth: 0\n"
        "      message_count: 2\n"
        "  custom_data:\n"
        "    robot: test-rig\n"
        "  future_extension:\n"
        "    vendor_payload: opaque-value\n");
    writeFile(source / "metadata.yaml", metadata);

    const auto inspection = lab::adapters::rosbag2::inspectRosbag2(source);
    require(inspection.success && inspection.metadata.present &&
                inspection.metadata.parsed && inspection.metadata.version == 5 &&
                inspection.metadata.storageIdentifier == "sqlite3" &&
                inspection.metadata.durationNanoseconds == 100 &&
                inspection.metadata.startingTimeNanoseconds == 100 &&
                inspection.metadata.messageCount == 3 &&
                inspection.metadata.rosDistro == "humble",
            "metadata.yaml stable fields are parsed without replacing SQLite truth");
    require(inspection.metadata.relativeFilePaths ==
                std::vector<std::string>{"bag_0.db3"} &&
                inspection.metadata.storageIdentifierMatches == true &&
                inspection.metadata.databaseFilesMatch == true &&
                inspection.metadata.messageCountMatches == true &&
                inspection.metadata.warnings.empty() &&
                inspection.metadata.sha256 ==
                    QCryptographicHash::hash(metadata, QCryptographicHash::Sha256)
                        .toHex()
                        .toStdString(),
            "metadata.yaml matches the inspected SQLite files and message count");

    const auto destination = root / "metadata_session";
    lab::adapters::rosbag2::Rosbag2ImportOptions metadataOptions{
        source, destination, "metadata", "0.14.0", {}};
    metadataOptions.includedTopics.push_back(
        {"/temperature", "std_msgs/msg/Float64"});
    const auto result =
        lab::adapters::rosbag2::importRosbag2(metadataOptions);
    require(result.success && result.messageCount == 2 &&
                result.metadata.messageCount == 3 &&
                result.metadata.messageCountMatches == true,
            "explicit Topic selection validates metadata against all SQLite messages");
    require(readFile(destination / "configuration" / "rosbag2_metadata.yaml") ==
                metadata,
            "complete metadata.yaml bytes, including QoS, custom data, and unknown "
            "fields, are preserved");

    const auto catalog = QJsonDocument::fromJson(
                             readFile(destination / "configuration" / "rosbag2.json"))
                             .object();
    const auto summary = catalog.value(QStringLiteral("metadata_yaml")).toObject();
    require(summary.value(QStringLiteral("validation_status")).toString() ==
                QStringLiteral("validated") &&
                summary.value(QStringLiteral("sha256")).toString().toStdString() ==
                    result.metadata.sha256 &&
                QCryptographicHash::hash(
                    readFile(destination / "configuration" /
                             "rosbag2_metadata.yaml"),
                    QCryptographicHash::Sha256)
                        .toHex()
                        .toStdString() == result.metadata.sha256 &&
                summary.value(QStringLiteral("validation"))
                    .toObject()
                    .value(QStringLiteral("database_files_match"))
                    .toBool(),
            "Session catalog records metadata identity and validation results");
    const auto sessionMetadata = QJsonDocument::fromJson(
                                     readFile(destination / "metadata.json"))
                                     .object();
    require(sessionMetadata.value(QStringLiteral("import"))
                    .toObject()
                    .value(QStringLiteral("metadata_yaml"))
                    .toObject()
                    .value(QStringLiteral("preserved_as"))
                    .toString() ==
                QStringLiteral("configuration/rosbag2_metadata.yaml"),
            "top-level Session metadata links the preserved rosbag2 metadata");

    const auto mismatchSource = root / "metadata_mismatch_bag";
    std::filesystem::create_directories(mismatchSource);
    createBag(QString::fromStdWString((mismatchSource / "actual.db3").wstring()),
              QStringLiteral("cdr"),
              {{1, 300, QByteArray::fromHex("01")}});
    createBag(QString::fromStdWString((root / "outside.db3").wstring()),
              QStringLiteral("cdr"),
              {{1, 301, QByteArray::fromHex("02")},
               {1, 302, QByteArray::fromHex("03")}});
    const QByteArray mismatchMetadata = QByteArrayLiteral(
        "rosbag2_bagfile_information:\n"
        "  version: 5\n"
        "  storage_identifier: mcap\n"
        "  message_count: 99\n"
        "  relative_file_paths:\n"
        "    - ../outside.db3\n"
        "    - /absolute.db3\n"
        "    - C:\\windows.db3\n"
        "    - \\\\server\\share.db3\n");
    writeFile(mismatchSource / "metadata.yaml", mismatchMetadata);
    const auto mismatchInspection =
        lab::adapters::rosbag2::inspectRosbag2(mismatchSource);
    require(mismatchInspection.success &&
                mismatchInspection.metadata.storageIdentifierMatches == false &&
                mismatchInspection.metadata.databaseFilesMatch == false &&
                mismatchInspection.metadata.messageCountMatches == false &&
                mismatchInspection.metadata.warnings.size() >= 4,
            "untrusted metadata mismatches become warnings while SQLite remains readable");
    const auto mismatchDestination = root / "metadata_mismatch_session";
    const auto mismatchResult = lab::adapters::rosbag2::importRosbag2(
        {mismatchSource, mismatchDestination, "metadata-mismatch", "0.14.0", {}});
    require(mismatchResult.success && mismatchResult.databaseCount == 1 &&
                mismatchResult.messageCount == 1 &&
                readFile(mismatchDestination / "configuration" /
                         "rosbag2_metadata.yaml") == mismatchMetadata,
            "unsafe metadata paths are preserved but cannot expand SQLite discovery");

    const auto singleDestination = root / "metadata_single_file_session";
    const auto singleResult = lab::adapters::rosbag2::importRosbag2(
        {mismatchSource / "actual.db3",
         singleDestination,
         "metadata-single-file",
         "0.14.0",
         {}});
    require(singleResult.success && singleResult.databaseCount == 1 &&
                singleResult.messageCount == 1 &&
                singleResult.metadata.databaseFilesMatch == false,
            "single-db3 import warns about unrelated sibling metadata without opening it");

    const auto invalidSource = root / "metadata_invalid_utf8_bag";
    std::filesystem::create_directories(invalidSource);
    createBag(QString::fromStdWString((invalidSource / "data.db3").wstring()),
              QStringLiteral("cdr"),
              {{1, 400, QByteArray::fromHex("01")}});
    QByteArray invalidMetadata("rosbag2_bagfile_information:\n  ros_distro: ");
    invalidMetadata.push_back(static_cast<char>(0xff));
    invalidMetadata.push_back('\n');
    writeFile(invalidSource / "metadata.yaml", invalidMetadata);
    const auto invalidDestination = root / "metadata_invalid_utf8_session";
    const auto invalidResult = lab::adapters::rosbag2::importRosbag2(
        {invalidSource, invalidDestination, "metadata-invalid", "0.14.0", {}});
    require(invalidResult.success && invalidResult.metadata.present &&
                !invalidResult.metadata.parsed &&
                !invalidResult.metadata.warnings.empty() &&
                readFile(invalidDestination / "configuration" /
                         "rosbag2_metadata.yaml") == invalidMetadata,
            "invalid UTF-8 metadata is losslessly preserved and ignored as authority");

    const auto snapshotSource = root / "metadata_snapshot_bag";
    std::filesystem::create_directories(snapshotSource);
    createBag(QString::fromStdWString((snapshotSource / "data.db3").wstring()),
              QStringLiteral("cdr"),
              {{1, 500, QByteArray::fromHex("01")},
               {1, 600, QByteArray::fromHex("02")}});
    const QByteArray initialMetadata = QByteArrayLiteral(
        "rosbag2_bagfile_information:\n"
        "  version: 5\n"
        "  storage_identifier: sqlite3\n"
        "  message_count: 2\n"
        "  relative_file_paths:\n"
        "    - data.db3\n");
    const QByteArray changedMetadata = QByteArrayLiteral(
        "rosbag2_bagfile_information:\n"
        "  version: 999\n"
        "  storage_identifier: mcap\n"
        "  message_count: 999\n");
    writeFile(snapshotSource / "metadata.yaml", initialMetadata);
    bool metadataChanged = false;
    const auto snapshotDestination = root / "metadata_snapshot_session";
    const auto snapshotResult = lab::adapters::rosbag2::importRosbag2(
        {snapshotSource, snapshotDestination, "metadata-snapshot", "0.14.0", {}},
        [&](std::uint64_t, std::uint64_t) {
            if (!metadataChanged) {
                writeFile(snapshotSource / "metadata.yaml", changedMetadata);
                metadataChanged = true;
            }
            return true;
        });
    const auto savedSnapshot = readFile(
        snapshotDestination / "configuration" / "rosbag2_metadata.yaml");
    require(snapshotResult.success && metadataChanged &&
                savedSnapshot == initialMetadata &&
                snapshotResult.metadata.sha256 ==
                    QCryptographicHash::hash(savedSnapshot,
                                             QCryptographicHash::Sha256)
                        .toHex()
                        .toStdString(),
            "metadata content and SHA-256 come from one immutable import snapshot");

    const auto duplicateSource = root / "metadata_duplicate_bag";
    std::filesystem::create_directories(duplicateSource);
    createBag(QString::fromStdWString((duplicateSource / "data.db3").wstring()),
              QStringLiteral("cdr"),
              {{1, 700, QByteArray::fromHex("01")}});
    const QByteArray duplicateMetadata = QByteArrayLiteral(
        "rosbag2_bagfile_information:\n"
        "  version: 5\n"
        "  version: 5\n"
        "  storage_identifier: sqlite3\n"
        "  message_count: 1\n"
        "  relative_file_paths:\n"
        "    - data.db3\n"
        "    - data.db3\n");
    writeFile(duplicateSource / "metadata.yaml", duplicateMetadata);
    const auto duplicateDestination = root / "metadata_duplicate_session";
    const auto duplicateResult = lab::adapters::rosbag2::importRosbag2(
        {duplicateSource,
         duplicateDestination,
         "metadata-duplicate",
         "0.14.0",
         {}});
    const auto duplicateSummary = QJsonDocument::fromJson(
                                      readFile(duplicateDestination /
                                               "configuration" / "rosbag2.json"))
                                      .object()
                                      .value(QStringLiteral("metadata_yaml"))
                                      .toObject();
    require(duplicateResult.success &&
                duplicateResult.metadata.storageIdentifierMatches == true &&
                duplicateResult.metadata.databaseFilesMatch == true &&
                duplicateResult.metadata.messageCountMatches == true &&
                !duplicateResult.metadata.warnings.empty() &&
                duplicateSummary.value(QStringLiteral("validation_status"))
                        .toString() == QStringLiteral("warning"),
            "duplicate stable fields cannot claim fully validated metadata");

    const auto incompleteSource = root / "metadata_incomplete_bag";
    std::filesystem::create_directories(incompleteSource);
    createBag(QString::fromStdWString((incompleteSource / "data.db3").wstring()),
              QStringLiteral("cdr"),
              {{1, 800, QByteArray::fromHex("01")}});
    writeFile(incompleteSource / "metadata.yaml",
              QByteArrayLiteral(
                  "rosbag2_bagfile_information:\n"
                  "  version: 5\n"));
    const auto incompleteDestination = root / "metadata_incomplete_session";
    const auto incompleteResult = lab::adapters::rosbag2::importRosbag2(
        {incompleteSource,
         incompleteDestination,
         "metadata-incomplete",
         "0.14.0",
         {}});
    const auto incompleteStatus = QJsonDocument::fromJson(
                                      readFile(incompleteDestination /
                                               "configuration" / "rosbag2.json"))
                                      .object()
                                      .value(QStringLiteral("metadata_yaml"))
                                      .toObject()
                                      .value(QStringLiteral("validation_status"))
                                      .toString();
    require(incompleteResult.success && incompleteStatus == QStringLiteral("parsed"),
            "missing stable fields cannot claim fully validated metadata");
}

void testCancellationAndValidation(const std::filesystem::path& root) {
    const auto bag = root / "single.db3";
    createBag(QString::fromStdWString(bag.wstring()),
              QStringLiteral("cdr"),
              {{1, 100, QByteArray::fromHex("0102")}});
    const auto cancelledDestination = root / "cancelled_session";
    const auto cancelled = lab::adapters::rosbag2::importRosbag2(
        {bag, cancelledDestination, "cancelled", "0.14.0", {}},
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
    const auto unsupportedInspection =
        lab::adapters::rosbag2::inspectRosbag2(unsupportedBag);
    require(unsupportedInspection.success && unsupportedInspection.topics.size() == 1 &&
                unsupportedInspection.topics.front().serializationFormat == "json",
            "inspection reports unsupported formats so the UI can disable them");
    const auto unsupported = lab::adapters::rosbag2::importRosbag2(
        {unsupportedBag, unsupportedDestination, "unsupported", "0.14.0", {}});
    require(!unsupported.success &&
                unsupported.error.find("serialization format") != std::string::npos,
            "non-CDR bag fails with a clear format error");
    require(!std::filesystem::exists(unsupportedDestination),
            "invalid input does not publish a partial Session");

    const auto oversizedMetadataSource = root / "oversized_metadata_bag";
    std::filesystem::create_directories(oversizedMetadataSource);
    createBag(QString::fromStdWString(
                  (oversizedMetadataSource / "data.db3").wstring()),
              QStringLiteral("cdr"),
              {{1, 100, QByteArray::fromHex("01")}});
    QFile oversizedMetadata(QString::fromStdWString(
        (oversizedMetadataSource / "metadata.yaml").wstring()));
    require(oversizedMetadata.open(QIODevice::WriteOnly) &&
                oversizedMetadata.resize(4 * 1024 * 1024 + 1),
            "oversized metadata fixture is created");
    oversizedMetadata.close();
    const auto oversizedInspection =
        lab::adapters::rosbag2::inspectRosbag2(oversizedMetadataSource);
    require(oversizedInspection.success && oversizedInspection.metadata.present &&
                !oversizedInspection.metadata.parsed &&
                !oversizedInspection.metadata.warnings.empty(),
            "oversized metadata is ignored without hiding the SQLite catalog");
    const auto oversizedDestination = root / "oversized_metadata_session";
    const auto oversizedResult = lab::adapters::rosbag2::importRosbag2(
        {oversizedMetadataSource,
         oversizedDestination,
         "oversized-metadata",
         "0.14.0",
         {}});
    require(oversizedResult.success &&
                !std::filesystem::exists(oversizedDestination / "configuration" /
                                         "rosbag2_metadata.yaml"),
            "metadata above the safety limit is neither loaded nor copied");

    const auto mixedSource = root / "mixed_serialization_bag";
    std::filesystem::create_directories(mixedSource);
    createBag(QString::fromStdWString((mixedSource / "cdr.db3").wstring()),
              QStringLiteral("cdr"),
              {{1, 100, QByteArray::fromHex("01")}});
    createBag(QString::fromStdWString((mixedSource / "json.db3").wstring()),
              QStringLiteral("json"),
              {{1, 200, QByteArrayLiteral("{}")}});
    const auto mixedDestination = root / "mixed_serialization_session";
    lab::adapters::rosbag2::Rosbag2ImportOptions mixedOptions{
        mixedSource, mixedDestination, "mixed", "0.14.0", {}};
    mixedOptions.includedTopics.push_back(
        {"/temperature", "std_msgs/msg/Float64"});
    const auto mixed = lab::adapters::rosbag2::importRosbag2(mixedOptions);
    require(mixed.success && mixed.topics.size() == 1 &&
                mixed.topics.front().serializationFormat == "cdr" &&
                mixed.messageCount == 1,
            "explicit selection imports only the supported CDR catalog row");

    const auto missingDestination = root / "missing_topic_session";
    lab::adapters::rosbag2::Rosbag2ImportOptions missingOptions{
        bag, missingDestination, "missing", "0.14.0", {}};
    missingOptions.includedTopics.push_back(
        {"/missing", "std_msgs/msg/String"});
    const auto missing = lab::adapters::rosbag2::importRosbag2(missingOptions);
    require(!missing.success && missing.error.find("no longer exist") != std::string::npos,
            "stale Topic selection fails explicitly");
    require(!std::filesystem::exists(missingDestination),
            "stale selection does not publish a partial Session");

    const auto occupied = root / "occupied_session";
    std::filesystem::create_directories(occupied);
    const auto existing = lab::adapters::rosbag2::importRosbag2(
        {bag, occupied, "occupied", "0.14.0", {}});
    require(!existing.success &&
                existing.error.find("already exists") != std::string::npos,
            "import never overwrites an existing destination");
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Lab Debugger rosbag2 tests"));
    try {
        if (argc == 4 && std::string_view(argv[1]) == "--verify-external-bag") {
            verifyExternalBag(toPath(QString::fromLocal8Bit(argv[2])),
                              toPath(QString::fromLocal8Bit(argv[3])));
            return 0;
        }
        require(argc == 1,
                "usage: lab_rosbag2_tests [--verify-external-bag BAG SESSION]");
        const auto root = toPath(QDir::currentPath()) /
                          ("lab-debugger-rosbag2-test-" +
                           QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString());
        std::filesystem::create_directories(root);
        testSplitBagImport(root);
        testStructuredImport(root);
        testMetadataPreservationAndValidation(root);
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

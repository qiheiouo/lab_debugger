#include "lab/adapters/rosbag2/rosbag2_importer.hpp"

#include "lab/adapters/rosbag2/cdr_field_mapper.hpp"

#include "lab/core/data_chunk.hpp"
#include "lab/core/raw_log_writer.hpp"

#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStringList>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <system_error>
#include <tuple>

namespace lab::adapters::rosbag2 {

std::string rosbag2SourceId(std::string_view topic, std::string_view messageType) {
    std::string result("rosbag2:");
    result.append(topic);
    result.append(" [");
    result.append(messageType);
    result.push_back(']');
    return result;
}

namespace {

constexpr std::size_t maximumTopicTextBytes = 64 * 1024;

QString fromPath(const std::filesystem::path& path) {
#ifdef _WIN32
    return QString::fromStdWString(path.wstring());
#else
    return QString::fromStdString(path.string());
#endif
}

std::string toUtf8(const QString& value) {
    const auto encoded = value.toUtf8();
    return {encoded.constData(), static_cast<std::size_t>(encoded.size())};
}

std::string pathText(const std::filesystem::path& path) {
    return toUtf8(fromPath(path));
}

std::string csvEscape(std::string_view value) {
    if (value.find_first_of(",\"\r\n") == std::string_view::npos) {
        return std::string(value);
    }
    std::string result;
    result.reserve(value.size() + 2);
    result.push_back('"');
    for (const auto character : value) {
        if (character == '"') result.push_back('"');
        result.push_back(character);
    }
    result.push_back('"');
    return result;
}

class TemporaryTree {
public:
    explicit TemporaryTree(std::filesystem::path path) : path_(std::move(path)) {}
    ~TemporaryTree() {
        if (!released_) {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    }
    void release() noexcept { released_ = true; }

private:
    std::filesystem::path path_;
    bool released_{};
};

std::vector<std::filesystem::path> databaseFiles(
    const std::filesystem::path& source,
    std::string& error) {
    std::error_code fileError;
    if (std::filesystem::is_regular_file(source, fileError)) {
        if (fromPath(source.extension()).compare(QStringLiteral(".db3"),
                                                 Qt::CaseInsensitive) != 0) {
            error = "rosbag2 source file must use the .db3 extension";
            return {};
        }
        return {source};
    }
    if (fileError || !std::filesystem::is_directory(source, fileError)) {
        error = "rosbag2 source does not exist or is not readable";
        return {};
    }

    std::vector<std::filesystem::path> result;
    for (std::filesystem::directory_iterator it(source, fileError), end;
         !fileError && it != end;
         it.increment(fileError)) {
        if (it->is_regular_file(fileError) &&
            fromPath(it->path().extension()).compare(QStringLiteral(".db3"),
                                                      Qt::CaseInsensitive) == 0) {
            result.push_back(it->path());
        }
    }
    if (fileError) {
        error = "cannot enumerate rosbag2 directory: " + fileError.message();
        return {};
    }
    std::sort(result.begin(), result.end());
    if (result.empty()) {
        error = "rosbag2 directory contains no .db3 files";
    }
    return result;
}

bool writeJson(const std::filesystem::path& path,
               const QJsonDocument& document,
               std::string& error) {
    QSaveFile output(fromPath(path));
    if (!output.open(QIODevice::WriteOnly)) {
        error = "cannot create " + pathText(path.filename());
        return false;
    }
    if (output.write(document.toJson(QJsonDocument::Indented)) < 0 || !output.commit()) {
        error = "cannot write " + pathText(path.filename());
        return false;
    }
    return true;
}

struct CursorRecord {
    lab::core::Timestamp timestamp{};
    std::int64_t rowId{};
    QString topicName;
    QString topicType;
    QByteArray payload;
};

bool topicSelected(const std::string& name,
                   const std::string& type,
                   const std::string& serializationFormat,
                   const std::vector<Rosbag2TopicSelector>& selection) {
    return selection.empty() ||
           (serializationFormat == "cdr" &&
            std::any_of(selection.begin(), selection.end(), [&](const auto& selected) {
                return selected.name == name && selected.type == type;
            }));
}

class DatabaseCursor {
public:
    DatabaseCursor(std::filesystem::path path, std::size_t order)
        : path_(std::move(path)), order_(order),
          connectionName_(QStringLiteral("lab-rosbag2-%1")
                              .arg(QUuid::createUuid().toString(QUuid::WithoutBraces))) {}

    ~DatabaseCursor() {
        query_.reset();
        if (database_.isValid()) {
            database_.close();
        }
        database_ = {};
        QSqlDatabase::removeDatabase(connectionName_);
    }

    bool open(std::string& error) {
        database_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName_);
        database_.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        database_.setDatabaseName(fromPath(path_));
        if (!database_.open()) {
            error = "cannot open rosbag2 database " + pathText(path_.filename()) + ": " +
                    toUtf8(database_.lastError().text());
            return false;
        }
        return true;
    }

    bool start(const std::vector<Rosbag2TopicSelector>& selection,
               std::string& error) {
        QString whereClause;
        if (!selection.empty()) {
            QSqlQuery topicQuery(database_);
            if (!topicQuery.exec(QStringLiteral(
                    "SELECT id, name, type, serialization_format FROM topics"))) {
                error = "cannot select rosbag2 topics from " + pathText(path_.filename()) +
                        ": " + toUtf8(topicQuery.lastError().text());
                return false;
            }
            QStringList ids;
            while (topicQuery.next()) {
                bool idOk = false;
                const auto id = topicQuery.value(0).toLongLong(&idOk);
                const auto name = toUtf8(topicQuery.value(1).toString());
                const auto type = toUtf8(topicQuery.value(2).toString());
                const auto format = toUtf8(topicQuery.value(3).toString());
                if (!idOk) {
                    error = "rosbag2 topic contains an invalid id";
                    return false;
                }
                if (topicSelected(name, type, format, selection)) {
                    ids.push_back(QString::number(id));
                }
            }
            if (topicQuery.lastError().isValid()) {
                error = "cannot read rosbag2 topics: " +
                        toUtf8(topicQuery.lastError().text());
                return false;
            }
            if (ids.empty()) {
                record_.reset();
                return true;
            }
            whereClause = QStringLiteral(" WHERE m.topic_id IN (%1)").arg(ids.join(','));
        }

        query_ = std::make_unique<QSqlQuery>(database_);
        query_->setForwardOnly(true);
        const auto statement =
            QStringLiteral("SELECT m.timestamp, m.id, t.name, t.type, m.data "
                           "FROM messages m JOIN topics t ON t.id = m.topic_id") +
            whereClause + QStringLiteral(" ORDER BY m.timestamp, m.id");
        if (!query_->exec(statement)) {
            error = "unsupported or damaged rosbag2 schema in " + pathText(path_.filename()) +
                    ": " + toUtf8(query_->lastError().text());
            return false;
        }
        return advance(error);
    }

    bool advance(std::string& error) {
        if (!query_ || !query_->next()) {
            if (query_ && query_->lastError().isValid()) {
                error = "cannot read rosbag2 messages from " + pathText(path_.filename()) +
                        ": " + toUtf8(query_->lastError().text());
                return false;
            }
            record_.reset();
            return true;
        }

        bool timestampOk = false;
        bool rowIdOk = false;
        CursorRecord record;
        record.timestamp = query_->value(0).toLongLong(&timestampOk);
        record.rowId = query_->value(1).toLongLong(&rowIdOk);
        record.topicName = query_->value(2).toString();
        record.topicType = query_->value(3).toString();
        record.payload = query_->value(4).toByteArray();
        if (!timestampOk || !rowIdOk || record.timestamp < 0 || record.topicName.isEmpty() ||
            record.topicType.isEmpty()) {
            error = "rosbag2 message contains an invalid timestamp, id, topic, or type";
            return false;
        }
        if (record.topicName.toUtf8().size() > maximumTopicTextBytes ||
            record.topicType.toUtf8().size() > maximumTopicTextBytes) {
            error = "rosbag2 topic name or type exceeds the safety limit";
            return false;
        }
        record_ = std::move(record);
        return true;
    }

    [[nodiscard]] bool hasRecord() const noexcept { return record_.has_value(); }
    [[nodiscard]] const CursorRecord& record() const { return *record_; }
    [[nodiscard]] std::size_t order() const noexcept { return order_; }
    [[nodiscard]] QSqlDatabase database() const { return database_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
    std::size_t order_{};
    QString connectionName_;
    QSqlDatabase database_;
    std::unique_ptr<QSqlQuery> query_;
    std::optional<CursorRecord> record_;
};

using TopicKey = std::tuple<std::string, std::string, std::string>;

bool inspectDatabase(DatabaseCursor& cursor,
                     std::map<TopicKey, std::uint64_t>& topics,
                     std::uint64_t& totalMessages,
                     std::uint64_t& totalBytes,
                     const std::vector<Rosbag2TopicSelector>& selection,
                     bool allowUnsupportedFormats,
                     std::string& error) {
    QSqlQuery query(cursor.database());
    if (!query.exec(QStringLiteral(
            "SELECT t.name, t.type, t.serialization_format, COUNT(m.id), "
            "COALESCE(SUM(LENGTH(m.data)), 0) "
            "FROM topics t LEFT JOIN messages m ON m.topic_id = t.id "
            "GROUP BY t.id, t.name, t.type, t.serialization_format"))) {
        error = "unsupported or damaged rosbag2 topic schema in " +
                pathText(cursor.path().filename()) + ": " + toUtf8(query.lastError().text());
        return false;
    }
    while (query.next()) {
        const auto name = toUtf8(query.value(0).toString());
        const auto type = toUtf8(query.value(1).toString());
        const auto format = toUtf8(query.value(2).toString());
        bool countOk = false;
        bool bytesOk = false;
        const auto count = query.value(3).toULongLong(&countOk);
        const auto bytes = query.value(4).toULongLong(&bytesOk);
        if (name.empty() || type.empty() || format.empty() || !countOk || !bytesOk) {
            error = "rosbag2 topic catalog contains invalid values";
            return false;
        }
        if (name.size() > maximumTopicTextBytes || type.size() > maximumTopicTextBytes) {
            error = "rosbag2 topic name or type exceeds the safety limit";
            return false;
        }
        if (!topicSelected(name, type, format, selection)) {
            continue;
        }
        if (!allowUnsupportedFormats && format != "cdr") {
            error = "unsupported rosbag2 serialization format '" + format +
                    "' for topic " + name;
            return false;
        }
        auto& topicTotal = topics[{name, type, format}];
        if (count > std::numeric_limits<std::uint64_t>::max() - topicTotal ||
            count > std::numeric_limits<std::uint64_t>::max() - totalMessages ||
            bytes > std::numeric_limits<std::uint64_t>::max() - totalBytes) {
            error = "rosbag2 catalog counters exceed the supported range";
            return false;
        }
        topicTotal += count;
        totalMessages += count;
        totalBytes += bytes;
    }
    if (query.lastError().isValid()) {
        error = "cannot read rosbag2 topic catalog: " + toUtf8(query.lastError().text());
        return false;
    }
    return true;
}

bool writeSessionFiles(const std::filesystem::path& root,
                       const Rosbag2ImportOptions& options,
                       const std::vector<std::filesystem::path>& databases,
                       const Rosbag2ImportResult& result,
                       std::string& error) {
    {
        std::ofstream frames(root / "frames.jsonl", std::ios::trunc);
        std::ofstream fields(root / "configuration" / "csv_fields.txt", std::ios::trunc);
        if (!frames || !fields) {
            error = "cannot create imported Session result files";
            return false;
        }
    }

    QJsonArray topicArray;
    QJsonArray sourceArray;
    for (const auto& topic : result.topics) {
        QJsonObject item;
        item.insert(QStringLiteral("name"), QString::fromUtf8(topic.name));
        item.insert(QStringLiteral("type"), QString::fromUtf8(topic.type));
        item.insert(QStringLiteral("serialization_format"),
                    QString::fromUtf8(topic.serializationFormat));
        item.insert(QStringLiteral("message_count"),
                    static_cast<qint64>(topic.messageCount));
        item.insert(QStringLiteral("field_mapping"),
                    topic.structuredFields ? QStringLiteral("built-in")
                                           : QStringLiteral("raw-only"));
        topicArray.push_back(item);

        QJsonObject source;
        source.insert(QStringLiteral("id"),
                      QString::fromStdString(rosbag2SourceId(topic.name, topic.type)));
        source.insert(QStringLiteral("type"), QStringLiteral("rosbag2-cdr"));
        source.insert(QStringLiteral("name"), QString::fromUtf8(topic.name));
        source.insert(QStringLiteral("message_type"), QString::fromUtf8(topic.type));
        sourceArray.push_back(source);
    }

    QJsonArray databaseArray;
    for (const auto& database : databases) {
        databaseArray.push_back(fromPath(database));
    }
    QJsonObject catalog;
    catalog.insert(QStringLiteral("format"), QStringLiteral("rosbag2-sqlite3"));
    catalog.insert(QStringLiteral("selection_mode"),
                   options.includedTopics.empty() ? QStringLiteral("all")
                                                  : QStringLiteral("explicit"));
    catalog.insert(QStringLiteral("source"), fromPath(options.source));
    catalog.insert(QStringLiteral("databases"), databaseArray);
    catalog.insert(QStringLiteral("topics"), topicArray);
    if (!writeJson(root / "configuration" / "rosbag2.json",
                   QJsonDocument(catalog),
                   error)) {
        return false;
    }

    QJsonObject event;
    event.insert(QStringLiteral("timestamp_ns"), result.firstTimestamp);
    event.insert(QStringLiteral("sequence"), 0);
    event.insert(QStringLiteral("source_id"), QStringLiteral("rosbag2-import"));
    event.insert(QStringLiteral("severity"), QStringLiteral("info"));
    event.insert(QStringLiteral("category"), QStringLiteral("import"));
    event.insert(QStringLiteral("message"),
                 QStringLiteral("Imported %1 messages and %2 numeric samples from %3 rosbag2 database(s)")
                     .arg(result.messageCount)
                     .arg(result.sampleCount)
                     .arg(result.databaseCount));
    QFile events(fromPath(root / "events.jsonl"));
    if (!events.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        events.write(QJsonDocument(event).toJson(QJsonDocument::Compact) + '\n') < 0) {
        error = "cannot write imported Session events";
        return false;
    }
    events.close();

    QJsonObject counts;
    counts.insert(QStringLiteral("raw_chunks"), static_cast<qint64>(result.messageCount));
    counts.insert(QStringLiteral("raw_bytes"), static_cast<qint64>(result.payloadBytes));
    counts.insert(QStringLiteral("samples"), static_cast<qint64>(result.sampleCount));
    counts.insert(QStringLiteral("frames"), 0);
    counts.insert(QStringLiteral("events"), 1);

    QJsonObject protocol;
    protocol.insert(QStringLiteral("name"), QString());
    protocol.insert(QStringLiteral("snapshot"), QJsonValue::Null);

    QJsonObject import;
    import.insert(QStringLiteral("format"), QStringLiteral("rosbag2-sqlite3"));
    import.insert(QStringLiteral("source"), fromPath(options.source));
    import.insert(QStringLiteral("database_count"), static_cast<qint64>(result.databaseCount));
    import.insert(QStringLiteral("selected_topic_count"),
                  static_cast<qint64>(result.topics.size()));
    import.insert(QStringLiteral("mapped_message_count"),
                  static_cast<qint64>(result.mappedMessageCount));
    import.insert(QStringLiteral("mapping_failure_count"),
                  static_cast<qint64>(result.mappingFailures));

    QJsonObject metadata;
    metadata.insert(QStringLiteral("format"), QStringLiteral("lab-debug-session"));
    metadata.insert(QStringLiteral("format_version"), 1);
    metadata.insert(QStringLiteral("status"), QStringLiteral("completed"));
    metadata.insert(QStringLiteral("name"), QString::fromUtf8(options.sessionName));
    metadata.insert(QStringLiteral("software_version"),
                    QString::fromUtf8(options.softwareVersion));
    metadata.insert(QStringLiteral("start_time_ns"), result.firstTimestamp);
    metadata.insert(QStringLiteral("end_time_ns"), result.lastTimestamp);
    metadata.insert(QStringLiteral("replay_mode"),
                    result.sampleCount > 0 ? QStringLiteral("rosbag2-structured")
                                           : QStringLiteral("raw-only"));
    metadata.insert(QStringLiteral("protocol"), protocol);
    metadata.insert(QStringLiteral("sources"), sourceArray);
    metadata.insert(QStringLiteral("counts"), counts);
    metadata.insert(QStringLiteral("import"), import);
    return writeJson(root / "metadata.json", QJsonDocument(metadata), error);
}

}  // namespace

Rosbag2InspectionResult inspectRosbag2(
    const std::filesystem::path& source,
    Rosbag2CancellationCallback cancelled) {
    Rosbag2InspectionResult result;
    auto fail = [&result](std::string message) {
        result.error = std::move(message);
        return result;
    };
    if (source.empty()) {
        return fail("rosbag2 source is required");
    }

    std::string error;
    const auto files = databaseFiles(source, error);
    if (files.empty()) {
        return fail(std::move(error));
    }
    result.databaseCount = files.size();

    std::map<TopicKey, std::uint64_t> topicCounts;
    for (std::size_t index = 0; index < files.size(); ++index) {
        if (cancelled && cancelled()) {
            result.cancelled = true;
            return fail("rosbag2 inspection cancelled");
        }
        DatabaseCursor cursor(files[index], index);
        if (!cursor.open(error) ||
            !inspectDatabase(cursor,
                             topicCounts,
                             result.messageCount,
                             result.payloadBytes,
                             {},
                             true,
                             error)) {
            return fail(std::move(error));
        }
    }
    if (cancelled && cancelled()) {
        result.cancelled = true;
        return fail("rosbag2 inspection cancelled");
    }
    for (const auto& [key, count] : topicCounts) {
        result.topics.push_back(
            {std::get<0>(key),
             std::get<1>(key),
             std::get<2>(key),
             count,
             hasStructuredCdrMapping(std::get<1>(key))});
    }
    result.success = true;
    return result;
}

Rosbag2ImportResult importRosbag2(const Rosbag2ImportOptions& options,
                                 Rosbag2ProgressCallback progress) {
    Rosbag2ImportResult result;
    result.sessionDirectory = options.destination;
    auto fail = [&result](std::string message) {
        result.error = std::move(message);
        return result;
    };

    if (options.source.empty() || options.destination.empty()) {
        return fail("rosbag2 source and Session destination are required");
    }
    std::error_code fileError;
    if (std::filesystem::exists(options.destination, fileError)) {
        return fail("Session destination already exists");
    }
    if (fileError) {
        return fail("cannot inspect Session destination: " + fileError.message());
    }

    std::string error;
    const auto files = databaseFiles(options.source, error);
    if (files.empty()) {
        return fail(std::move(error));
    }
    result.databaseCount = files.size();

    auto parent = options.destination.parent_path();
    if (parent.empty()) {
        parent = std::filesystem::current_path(fileError);
    }
    std::filesystem::create_directories(parent, fileError);
    if (fileError) {
        return fail("cannot create Session parent directory: " + fileError.message());
    }
    const auto temporaryName =
        ".lab-debugger-rosbag2-" +
        toUtf8(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const auto temporaryRoot = parent / temporaryName;
    TemporaryTree temporary(temporaryRoot);
    for (const auto* name : {"raw", "protocol", "configuration"}) {
        std::filesystem::create_directories(temporaryRoot / name, fileError);
        if (fileError) {
            return fail("cannot initialize imported Session: " + fileError.message());
        }
    }

    std::vector<std::unique_ptr<DatabaseCursor>> cursors;
    std::map<TopicKey, std::uint64_t> topicCounts;
    for (std::size_t index = 0; index < files.size(); ++index) {
        auto cursor = std::make_unique<DatabaseCursor>(files[index], index);
        if (!cursor->open(error) ||
            !inspectDatabase(*cursor,
                             topicCounts,
                             result.messageCount,
                             result.payloadBytes,
                             options.includedTopics,
                             false,
                             error) ||
            !cursor->start(options.includedTopics, error)) {
            return fail(std::move(error));
        }
        cursors.push_back(std::move(cursor));
    }
    if (result.messageCount > static_cast<std::uint64_t>(
                                  std::numeric_limits<qint64>::max()) ||
        result.payloadBytes > static_cast<std::uint64_t>(
                                  std::numeric_limits<qint64>::max())) {
        return fail("rosbag2 size exceeds the Session metadata range");
    }
    for (const auto& [key, count] : topicCounts) {
        result.topics.push_back({std::get<0>(key),
                                 std::get<1>(key),
                                 std::get<2>(key),
                                 count,
                                 hasStructuredCdrMapping(std::get<1>(key))});
    }
    if (!options.includedTopics.empty()) {
        std::set<std::pair<std::string, std::string>> requested;
        std::set<std::pair<std::string, std::string>> found;
        for (const auto& selected : options.includedTopics) {
            if (selected.name.empty() || selected.type.empty()) {
                return fail("selected rosbag2 topic name and type are required");
            }
            requested.emplace(selected.name, selected.type);
        }
        for (const auto& topic : result.topics) {
            found.emplace(topic.name, topic.type);
        }
        if (requested != found) {
            return fail("one or more selected rosbag2 topics no longer exist");
        }
    }

    lab::core::RawLogWriter writer;
    if (!writer.open(temporaryRoot / "raw" / "stream.ldraw")) {
        return fail(writer.error());
    }
    std::ofstream values(temporaryRoot / "values.csv", std::ios::trunc);
    if (!values) {
        return fail("cannot create imported Session values.csv");
    }
    values << "timestamp_ns,source_id,sequence,field,value,unit\n";

    std::uint64_t imported{};
    while (true) {
        DatabaseCursor* selected{};
        for (const auto& cursor : cursors) {
            if (!cursor->hasRecord()) {
                continue;
            }
            if (selected == nullptr ||
                std::tuple(cursor->record().timestamp,
                           cursor->order(),
                           cursor->record().rowId) <
                    std::tuple(selected->record().timestamp,
                               selected->order(),
                               selected->record().rowId)) {
                selected = cursor.get();
            }
        }
        if (selected == nullptr) {
            break;
        }
        if (progress && !progress(imported, result.messageCount)) {
            result.cancelled = true;
            writer.close();
            return fail("rosbag2 import cancelled");
        }

        const auto& record = selected->record();
        const auto topic = toUtf8(record.topicName);
        const auto messageType = toUtf8(record.topicType);
        lab::core::DataChunk chunk;
        chunk.sourceId = rosbag2SourceId(topic, messageType);
        chunk.sourceTimestamp = record.timestamp;
        chunk.receiveTimestamp = record.timestamp;
        chunk.sequence = imported;
        chunk.direction = lab::core::Direction::Rx;
        chunk.payload.assign(record.payload.cbegin(), record.payload.cend());
        if (!writer.write(chunk)) {
            return fail(writer.error());
        }
        const auto mapping = mapStructuredCdrFields(messageType, chunk.payload);
        if (mapping.supported) {
            if (mapping.success) {
                ++result.mappedMessageCount;
                const auto sampleTimestamp = mapping.sourceTimestamp != 0
                                                 ? mapping.sourceTimestamp
                                                 : record.timestamp;
                for (const auto& field : mapping.fields) {
                    if (result.sampleCount == std::numeric_limits<std::uint64_t>::max()) {
                        return fail("rosbag2 structured sample count exceeds the supported range");
                    }
                    values << sampleTimestamp << ',' << csvEscape(chunk.sourceId) << ','
                           << imported << ',' << csvEscape(topic + "." + field.path) << ','
                           << std::setprecision(17) << field.value << ','
                           << csvEscape(field.unit) << '\n';
                    ++result.sampleCount;
                }
                if (!values) {
                    return fail("cannot write imported Session values.csv");
                }
            } else {
                ++result.mappingFailures;
            }
        }
        if (imported == 0) {
            result.firstTimestamp = record.timestamp;
        }
        result.lastTimestamp = record.timestamp;
        ++imported;
        if (!selected->advance(error)) {
            return fail(std::move(error));
        }
    }
    if (imported != result.messageCount) {
        return fail("rosbag2 message count changed while importing");
    }
    if (!writer.close()) {
        return fail(writer.error());
    }
    values.flush();
    if (!values) {
        return fail("cannot finalize imported Session values.csv");
    }
    values.close();
    if (result.sampleCount > static_cast<std::uint64_t>(
                                 std::numeric_limits<qint64>::max())) {
        return fail("rosbag2 structured sample count exceeds the Session metadata range");
    }
    if (!writeSessionFiles(temporaryRoot, options, files, result, error)) {
        return fail(std::move(error));
    }

    std::filesystem::rename(temporaryRoot, options.destination, fileError);
    if (fileError) {
        return fail("cannot finalize imported Session: " + fileError.message());
    }
    temporary.release();
    result.success = true;
    if (progress) {
        progress(result.messageCount, result.messageCount);
    }
    return result;
}

}  // namespace lab::adapters::rosbag2

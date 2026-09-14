#include "competition/session_summary.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace lab::competition {
namespace {

struct FieldStatistics {
    QString sourceId;
    QString field;
    QString unit;
    quint64 count{};
    double minimum{};
    double maximum{};
    double mean{};
    double squaredDifferenceSum{};
    double first{};
    double last{};
    qint64 firstTimestamp{};
    qint64 lastTimestamp{};

    void add(qint64 timestamp, double value) noexcept {
        if (count == 0) {
            minimum = maximum = first = last = value;
            firstTimestamp = lastTimestamp = timestamp;
            mean = value;
            count = 1;
            return;
        }
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
        last = value;
        lastTimestamp = timestamp;
        ++count;
        const auto delta = value - mean;
        mean += delta / static_cast<double>(count);
        squaredDifferenceSum += delta * (value - mean);
    }
};

std::optional<QStringList> parseCsvLine(QByteArray line) {
    while (line.endsWith('\n') || line.endsWith('\r')) line.chop(1);
    const auto text = QString::fromUtf8(line);
    QStringList fields;
    QString current;
    bool quoted = false;
    for (qsizetype index = 0; index < text.size(); ++index) {
        const auto character = text[index];
        if (quoted) {
            if (character == u'"') {
                if (index + 1 < text.size() && text[index + 1] == u'"') {
                    current += u'"';
                    ++index;
                } else {
                    quoted = false;
                }
            } else {
                current += character;
            }
            continue;
        }
        if (character == u',' ) {
            fields.push_back(current);
            current.clear();
        } else if (character == u'"' && current.isEmpty()) {
            quoted = true;
        } else {
            current += character;
        }
        if (fields.size() > 32) return std::nullopt;
    }
    if (quoted) return std::nullopt;
    fields.push_back(current);
    return fields;
}

QString timestampText(const QJsonObject& object, const QByteArray& rawLine) {
    static const QRegularExpression expression(
        QStringLiteral("\\\"timestamp_ns\\\"\\s*:\\s*(-?[0-9]+)"));
    const auto match = expression.match(QString::fromUtf8(rawLine));
    if (match.hasMatch()) return match.captured(1);
    const auto value = object.value(QStringLiteral("timestamp_ns"));
    if (value.isString()) return value.toString();
    if (value.isDouble()) {
        return QString::number(static_cast<qint64>(value.toDouble()));
    }
    return {};
}

QString integerText(const QByteArray& json, const QString& name) {
    const QRegularExpression expression(
        QStringLiteral("\\\"%1\\\"\\s*:\\s*(-?[0-9]+)")
            .arg(QRegularExpression::escape(name)));
    const auto match = expression.match(QString::fromUtf8(json));
    return match.hasMatch() ? match.captured(1) : QString{};
}

void discardLineRemainder(QFile& file, qint64 maximumLineBytes) {
    while (!file.atEnd()) {
        const auto remainder = file.readLine(maximumLineBytes + 1);
        if (remainder.endsWith('\n')) break;
    }
}

bool readMetadata(const QString& path,
                  QJsonObject& output,
                  QByteArray& raw,
                  QString& error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QObject::tr("无法读取 metadata.json");
        return false;
    }
    const auto expectedSize = file.size();
    if (expectedSize < 0 || expectedSize > 1024 * 1024) {
        error = QObject::tr("metadata.json 超过 1 MiB 限制");
        return false;
    }
    raw = file.readAll();
    if (file.error() != QFileDevice::NoError || raw.size() != expectedSize) {
        error = QObject::tr("metadata.json 未能完整读取");
        return false;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(raw, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        error = QObject::tr("metadata.json 不是有效 JSON");
        return false;
    }
    output = document.object();
    return true;
}

QJsonArray sourceSummary(const QJsonObject& metadata) {
    QJsonArray result;
    const auto sources = metadata.value(QStringLiteral("sources")).toArray();
    for (qsizetype index = 0; index < sources.size() && index < 32; ++index) {
        const auto source = sources[index].toObject();
        QJsonObject item;
        item.insert(QStringLiteral("id"), source.value(QStringLiteral("id")));
        item.insert(QStringLiteral("type"), source.value(QStringLiteral("type")));
        item.insert(QStringLiteral("name"), source.value(QStringLiteral("name")));
        result.push_back(item);
    }
    return result;
}

}  // namespace

SessionSummaryResult SessionSummaryBuilder::build(
    const QString& sessionDirectory,
    const SessionSummaryLimits& limits) {
    SessionSummaryResult result;
    if (limits.maximumValueRows == 0 || limits.maximumFields == 0 ||
        limits.maximumEvents == 0 || limits.maximumLineBytes < 128 ||
        limits.maximumLineBytes > 1024 * 1024 ||
        limits.maximumOutputBytes < 128 ||
        limits.maximumOutputBytes > 1024 * 1024) {
        result.error = QObject::tr("Session 摘要限制参数无效");
        return result;
    }
    const QFileInfo directoryInfo(sessionDirectory);
    if (!directoryInfo.exists() || !directoryInfo.isDir()) {
        result.error = QObject::tr("请选择有效的 Session 目录");
        return result;
    }

    const QDir directory(directoryInfo.absoluteFilePath());
    QJsonObject metadata;
    QByteArray rawMetadata;
    if (!readMetadata(directory.filePath(QStringLiteral("metadata.json")),
                      metadata,
                      rawMetadata,
                      result.error)) {
        return result;
    }

    QFile values(directory.filePath(QStringLiteral("values.csv")));
    if (!values.open(QIODevice::ReadOnly | QIODevice::Text)) {
        result.error = QObject::tr("Session 中缺少可读取的 values.csv");
        return result;
    }
    const auto headerBytes = values.readLine(limits.maximumLineBytes + 1);
    if (headerBytes.isEmpty() || headerBytes.size() > limits.maximumLineBytes) {
        result.error = QObject::tr("values.csv 表头缺失或过长");
        return result;
    }
    auto normalizedHeader = headerBytes;
    if (normalizedHeader.startsWith("\xEF\xBB\xBF")) {
        normalizedHeader.remove(0, 3);
    }
    const auto header = parseCsvLine(normalizedHeader);
    if (!header) {
        result.error = QObject::tr("values.csv 表头格式无效");
        return result;
    }
    const auto timestampIndex = header->indexOf(QStringLiteral("timestamp_ns"));
    const auto sourceIndex = header->indexOf(QStringLiteral("source_id"));
    const auto fieldIndex = header->indexOf(QStringLiteral("field"));
    const auto valueIndex = header->indexOf(QStringLiteral("value"));
    const auto unitIndex = header->indexOf(QStringLiteral("unit"));
    if (timestampIndex < 0 || sourceIndex < 0 || fieldIndex < 0 ||
        valueIndex < 0 || unitIndex < 0) {
        result.error = QObject::tr("values.csv 缺少时间、来源、字段、数值或单位列");
        return result;
    }
    const auto requiredIndex = std::max(
        {timestampIndex, sourceIndex, fieldIndex, valueIndex, unitIndex});

    QMap<QString, FieldStatistics> fields;
    while (!values.atEnd() &&
           result.valueRowsScanned < static_cast<quint64>(limits.maximumValueRows)) {
        const auto line = values.readLine(limits.maximumLineBytes + 1);
        if (line.size() > limits.maximumLineBytes ||
            (!line.endsWith('\n') && !values.atEnd())) {
            ++result.malformedRows;
            if (!line.endsWith('\n')) {
                discardLineRemainder(values, limits.maximumLineBytes);
            }
            continue;
        }
        ++result.valueRowsScanned;
        const auto columns = parseCsvLine(line);
        if (!columns || columns->size() <= requiredIndex) {
            ++result.malformedRows;
            continue;
        }
        bool timestampValid = false;
        bool valueValid = false;
        const auto timestamp = (*columns)[timestampIndex].toLongLong(&timestampValid);
        const auto value = (*columns)[valueIndex].toDouble(&valueValid);
        if (!timestampValid || timestamp <= 0 || !valueValid ||
            !std::isfinite(value)) {
            ++result.malformedRows;
            continue;
        }
        const auto sourceId = (*columns)[sourceIndex].left(1024);
        const auto field = (*columns)[fieldIndex].left(2048);
        const auto unit = (*columns)[unitIndex].left(128);
        const auto key = sourceId + QChar(0x1f) + field + QChar(0x1f) + unit;
        auto iterator = fields.find(key);
        if (iterator == fields.end()) {
            if (fields.size() >= static_cast<qsizetype>(limits.maximumFields)) {
                ++result.omittedFields;
                continue;
            }
            FieldStatistics statistics;
            statistics.sourceId = sourceId;
            statistics.field = field;
            statistics.unit = unit;
            iterator = fields.insert(key, statistics);
        }
        iterator->add(timestamp, value);
    }
    result.truncated = !values.atEnd();
    if (values.error() != QFileDevice::NoError) {
        result.error = QObject::tr("读取 values.csv 时发生错误");
        return result;
    }

    QJsonArray fieldArray;
    for (const auto& statistics : fields) {
        if (statistics.count == 0) continue;
        QJsonObject item;
        item.insert(QStringLiteral("source_id"), statistics.sourceId);
        item.insert(QStringLiteral("field"), statistics.field);
        item.insert(QStringLiteral("unit"), statistics.unit);
        item.insert(QStringLiteral("count"),
                    static_cast<qint64>(statistics.count));
        item.insert(QStringLiteral("min"), statistics.minimum);
        item.insert(QStringLiteral("max"), statistics.maximum);
        item.insert(QStringLiteral("mean"), statistics.mean);
        item.insert(QStringLiteral("stddev"),
                    statistics.count > 1
                        ? std::sqrt(statistics.squaredDifferenceSum /
                                    static_cast<double>(statistics.count - 1))
                        : 0.0);
        item.insert(QStringLiteral("first"), statistics.first);
        item.insert(QStringLiteral("last"), statistics.last);
        item.insert(QStringLiteral("change"), statistics.last - statistics.first);
        item.insert(QStringLiteral("first_timestamp_ns"),
                    QString::number(statistics.firstTimestamp));
        item.insert(QStringLiteral("last_timestamp_ns"),
                    QString::number(statistics.lastTimestamp));
        fieldArray.push_back(item);
    }

    QJsonArray eventsArray;
    QFile events(directory.filePath(QStringLiteral("events.jsonl")));
    quint64 omittedEvents = 0;
    const auto eventsExist = QFileInfo::exists(events.fileName());
    if (eventsExist && !events.open(QIODevice::ReadOnly | QIODevice::Text)) {
        result.error = QObject::tr("events.jsonl 存在但无法读取");
        return result;
    }
    if (events.isOpen()) {
        while (!events.atEnd()) {
            const auto line = events.readLine(limits.maximumLineBytes + 1);
            if (line.size() > limits.maximumLineBytes ||
                (!line.endsWith('\n') && !events.atEnd())) {
                ++omittedEvents;
                if (!line.endsWith('\n')) {
                    discardLineRemainder(events, limits.maximumLineBytes);
                }
                continue;
            }
            QJsonParseError parseError;
            const auto document = QJsonDocument::fromJson(line, &parseError);
            if (parseError.error != QJsonParseError::NoError ||
                !document.isObject()) {
                ++omittedEvents;
                continue;
            }
            const auto event = document.object();
            const auto category = event.value(QStringLiteral("category")).toString();
            if (category != QStringLiteral("marker") &&
                category != QStringLiteral("alert") &&
                category != QStringLiteral("clock_sync")) {
                continue;
            }
            if (eventsArray.size() >=
                static_cast<qsizetype>(limits.maximumEvents)) {
                ++omittedEvents;
                continue;
            }
            QJsonObject item;
            item.insert(QStringLiteral("timestamp_ns"), timestampText(event, line));
            item.insert(QStringLiteral("category"), category);
            item.insert(QStringLiteral("severity"),
                        event.value(QStringLiteral("severity")).toString().left(64));
            item.insert(QStringLiteral("source_id"),
                        event.value(QStringLiteral("source_id")).toString().left(1024));
            item.insert(QStringLiteral("message"),
                        event.value(QStringLiteral("message")).toString().left(2048));
            eventsArray.push_back(item);
        }
        if (events.error() != QFileDevice::NoError) {
            result.error = QObject::tr("读取 events.jsonl 时发生错误");
            return result;
        }
    }

    QJsonObject session;
    session.insert(QStringLiteral("directory_name"), directoryInfo.fileName());
    session.insert(QStringLiteral("session_name"),
                   metadata.value(QStringLiteral("session_name"))
                       .toString()
                       .left(256));
    session.insert(QStringLiteral("data_origin"),
                   metadata.value(QStringLiteral("data_origin"))
                       .toString()
                       .left(128));
    session.insert(QStringLiteral("status"),
                   metadata.value(QStringLiteral("status")).toString());
    session.insert(QStringLiteral("software_version"),
                   metadata.value(QStringLiteral("software_version")).toString());
    session.insert(QStringLiteral("start_time_ns"),
                   integerText(rawMetadata, QStringLiteral("start_time_ns")));
    session.insert(QStringLiteral("end_time_ns"),
                   integerText(rawMetadata, QStringLiteral("end_time_ns")));
    session.insert(QStringLiteral("sources"), sourceSummary(metadata));
    session.insert(QStringLiteral("value_rows_scanned"),
                   static_cast<qint64>(result.valueRowsScanned));
    session.insert(QStringLiteral("malformed_rows"),
                   static_cast<qint64>(result.malformedRows));
    session.insert(QStringLiteral("scan_truncated"), result.truncated);
    session.insert(QStringLiteral("omitted_field_rows"),
                   static_cast<qint64>(result.omittedFields));
    session.insert(QStringLiteral("omitted_events"),
                   static_cast<qint64>(omittedEvents));

    QJsonObject root;
    root.insert(QStringLiteral("format_version"), 1);
    root.insert(QStringLiteral("privacy_note"),
                QStringLiteral("仅含本地聚合统计与少量事件，不含原始 CDR/字节流"));
    root.insert(QStringLiteral("session"), session);
    root.insert(QStringLiteral("fields"), fieldArray);
    root.insert(QStringLiteral("events"), eventsArray);
    result.json = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (result.json.size() > limits.maximumOutputBytes) {
        result.error = QObject::tr("本地摘要超过 96 KiB，请减少字段后重试");
        result.json.clear();
        return result;
    }
    result.success = true;
    return result;
}

}  // namespace lab::competition

#include "app/serial_session.hpp"

#include "lab/adapters/rosbag2/cdr_field_mapper.hpp"
#include "lab/core/protocol_json_loader.hpp"
#include "lab/core/timestamp.hpp"

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QSysInfo>
#include <QVariantMap>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>

namespace lab::app {
namespace {

constexpr std::string_view remoteAgentSourceKey = "remote_agent";
constexpr std::string_view serialSourcePrefix = "serial:";
constexpr std::string_view tcpClientSourcePrefix = "tcp-client:";
constexpr std::string_view tcpServerSourcePrefix = "tcp-server:";
constexpr std::string_view udpSourcePrefix = "udp:";
constexpr std::size_t maximumLocalSources = 16;
constexpr qint64 maximumDerivedConfigurationBytes = 1024 * 1024;
constexpr qsizetype maximumDerivedDefinitions = 128;
constexpr qint64 maximumProtocolDefinitionBytes = 1024 * 1024;
constexpr qint64 maximumSessionMetadataBytes = 4 * 1024 * 1024;
constexpr qsizetype maximumParserSources = 64;
constexpr qsizetype maximumCsvFields = 256;
constexpr qsizetype maximumCsvFieldBytes = 1024;
constexpr qint64 maximumAlertConfigurationBytes = 1024 * 1024;
constexpr qsizetype maximumAlertDefinitions = 128;
constexpr qint64 maximumHealthAlertConfigurationBytes = 1024 * 1024;
constexpr qsizetype maximumHealthAlertDefinitions = 128;
constexpr qint64 maximumAlertWindowMs = 86'400'000;
constexpr qint64 maximumEventLogBytes = 64 * 1024 * 1024;
constexpr qint64 maximumEventLineBytes = 64 * 1024;
constexpr qsizetype maximumTimelineEvents = 10'000;

std::string utf8String(const QString& value) {
    const auto bytes = value.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

bool containsControlCharacter(const QString& value) {
    return std::any_of(value.cbegin(), value.cend(), [](QChar character) {
        const auto code = character.unicode();
        return code < 0x20 || code == 0x7f;
    });
}

std::optional<std::vector<std::string>> validatedCsvFields(
    const QStringList& fields,
    QStringList& issues) {
    if (fields.size() > maximumCsvFields) {
        issues.push_back(QObject::tr("CSV 字段不能超过 256 项"));
        return std::nullopt;
    }
    std::set<std::string> unique;
    std::vector<std::string> result;
    result.reserve(static_cast<std::size_t>(fields.size()));
    for (qsizetype index = 0; index < fields.size(); ++index) {
        const auto cleaned = fields[index].trimmed();
        const auto bytes = cleaned.toUtf8();
        if (bytes.isEmpty() || bytes.size() > maximumCsvFieldBytes ||
            containsControlCharacter(cleaned)) {
            issues.push_back(QObject::tr("第 %1 个 CSV 字段为空、过长或包含控制字符")
                                 .arg(index + 1));
            continue;
        }
        std::string name(bytes.constData(), static_cast<std::size_t>(bytes.size()));
        if (!unique.insert(name).second) {
            issues.push_back(QObject::tr("CSV 字段名重复：%1").arg(cleaned));
            continue;
        }
        result.push_back(std::move(name));
    }
    if (result.empty() && issues.empty()) {
        issues.push_back(QObject::tr("至少需要一个 CSV 字段"));
    }
    return issues.empty() ? std::optional(std::move(result)) : std::nullopt;
}

struct LoadedProtocolDefinition {
    lab::core::ProtocolDefinition definition;
    std::string json;
    QStringList numericFields;
};

struct RestoredParserConfiguration {
    QString sourceId;
    std::vector<std::string> csvFields;
    std::optional<lab::core::ProtocolDefinition> protocolDefinition;
    std::string protocolName;
    std::string protocolJson;
    QStringList visibleFields;
};

std::optional<LoadedProtocolDefinition> readProtocolDefinition(
    const QString& path,
    QStringList& issues) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        issues.push_back(QObject::tr("无法打开协议文件：%1").arg(file.errorString()));
        return std::nullopt;
    }
    const auto declaredSize = file.size();
    if (declaredSize <= 0 || declaredSize > maximumProtocolDefinitionBytes) {
        issues.push_back(QObject::tr("协议文件为空或超过 1 MiB 安全限制"));
        return std::nullopt;
    }
    const auto data = file.readAll();
    if (file.error() != QFileDevice::NoError || data.size() != declaredSize) {
        issues.push_back(QObject::tr("协议文件读取不完整"));
        return std::nullopt;
    }
    const auto loaded = lab::core::loadProtocolJson(
        std::string_view(data.constData(), static_cast<std::size_t>(data.size())));
    if (!loaded.success()) {
        issues.reserve(static_cast<qsizetype>(loaded.issues.size()));
        for (const auto& issue : loaded.issues) {
            issues.push_back(QStringLiteral("[%1] %2")
                                 .arg(QString::fromStdString(issue.code),
                                      QString::fromStdString(issue.message)));
        }
        return std::nullopt;
    }
    QStringList numericFields;
    for (const auto& field : loaded.definition->fields) {
        if (field.type != lab::core::FieldType::ByteArray) {
            numericFields.push_back(QString::fromStdString(field.name));
        }
    }
    return LoadedProtocolDefinition{
        std::move(*loaded.definition),
        std::string(data.constData(), static_cast<std::size_t>(data.size())),
        std::move(numericFields)};
}

void updateLatestTimestamp(std::atomic<lab::core::Timestamp>& target,
                           lab::core::Timestamp value) {
    auto previous = target.load();
    while (value > previous &&
           !target.compare_exchange_weak(previous, value)) {
    }
}

QVariantMap eventToVariant(const lab::core::SessionEvent& event) {
    QVariantMap result;
    result.insert(QStringLiteral("timestamp_ns"), QVariant::fromValue(event.timestamp));
    result.insert(QStringLiteral("source_id"), QString::fromStdString(event.sourceId));
    result.insert(QStringLiteral("sequence"), QVariant::fromValue(event.sequence));
    result.insert(QStringLiteral("severity"), QString::fromStdString(event.severity));
    result.insert(QStringLiteral("category"), QString::fromStdString(event.category));
    result.insert(QStringLiteral("message"), QString::fromStdString(event.message));
    return result;
}

QVariantList derivedDefinitionsToVariant(
    const std::vector<lab::core::DerivedFieldDefinition>& definitions) {
    QVariantList result;
    result.reserve(static_cast<qsizetype>(definitions.size()));
    for (const auto& definition : definitions) {
        QVariantMap value;
        value.insert(QStringLiteral("name"), QString::fromStdString(definition.name));
        value.insert(QStringLiteral("expression"),
                     QString::fromStdString(definition.expression));
        value.insert(QStringLiteral("unit"), QString::fromStdString(definition.unit));
        result.push_back(value);
    }
    return result;
}

QVariantList alertDefinitionsToVariant(
    const std::vector<lab::core::ThresholdAlertDefinition>& definitions) {
    QVariantList result;
    result.reserve(static_cast<qsizetype>(definitions.size()));
    for (const auto& definition : definitions) {
        QVariantMap value;
        value.insert(QStringLiteral("name"), QString::fromStdString(definition.name));
        value.insert(QStringLiteral("field"), QString::fromStdString(definition.field));
        value.insert(QStringLiteral("comparison"),
                     QString::fromStdString(lab::core::toString(definition.comparison)));
        value.insert(QStringLiteral("threshold"), definition.threshold);
        value.insert(QStringLiteral("hysteresis"), definition.hysteresis);
        value.insert(QStringLiteral("duration_ms"), definition.durationNs / 1'000'000);
        value.insert(QStringLiteral("message"), QString::fromStdString(definition.message));
        result.push_back(value);
    }
    return result;
}

QVariantList healthAlertDefinitionsToVariant(
    const std::vector<lab::core::HealthAlertDefinition>& definitions) {
    QVariantList result;
    result.reserve(static_cast<qsizetype>(definitions.size()));
    for (const auto& definition : definitions) {
        QVariantMap value;
        value.insert(QStringLiteral("name"), QString::fromStdString(definition.name));
        value.insert(QStringLiteral("source_id"),
                     QString::fromStdString(definition.sourceId));
        value.insert(QStringLiteral("kind"),
                     QString::fromStdString(lab::core::toString(definition.kind)));
        value.insert(QStringLiteral("error_count"),
                     QVariant::fromValue<qulonglong>(definition.errorCount));
        value.insert(QStringLiteral("window_ms"), definition.windowNs / 1'000'000);
        value.insert(QStringLiteral("message"),
                     QString::fromStdString(definition.message));
        result.push_back(value);
    }
    return result;
}

std::optional<lab::core::Timestamp> parseTimestampField(std::string_view line) {
    constexpr std::string_view key = "\"timestamp_ns\"";
    std::size_t position = 0;
    while (position < line.size() &&
           (line[position] == ' ' || line[position] == '\t')) {
        ++position;
    }
    if (position >= line.size() || line[position++] != '{') return std::nullopt;
    while (position < line.size() &&
           (line[position] == ' ' || line[position] == '\t')) {
        ++position;
    }
    if (line.substr(position, key.size()) != key) return std::nullopt;
    position = line.find(':', position + key.size());
    if (position == std::string_view::npos) return std::nullopt;
    ++position;
    while (position < line.size() &&
           (line[position] == ' ' || line[position] == '\t')) {
        ++position;
    }
    lab::core::Timestamp result{};
    const auto parsed = std::from_chars(line.data() + position,
                                        line.data() + line.size(),
                                        result);
    if (parsed.ec != std::errc{} || parsed.ptr == line.data() + position) {
        return std::nullopt;
    }
    return result;
}

bool sameSettings(const lab::adapters::serial::SerialSettings& left,
                  const lab::adapters::serial::SerialSettings& right) {
    return left.portName == right.portName && left.baudRate == right.baudRate &&
           left.dataBits == right.dataBits && left.stopBits == right.stopBits &&
           left.parity == right.parity && left.flowControl == right.flowControl;
}

bool sameSettings(const lab::adapters::network::NetworkSettings& left,
                  const lab::adapters::network::NetworkSettings& right) {
    return left.mode == right.mode && left.remoteHost == right.remoteHost &&
           left.remotePort == right.remotePort && left.bindAddress == right.bindAddress &&
           left.localPort == right.localPort;
}

bool sameSettings(const lab::adapters::remote_agent::RemoteAgentSettings& left,
                  const lab::adapters::remote_agent::RemoteAgentSettings& right) {
    return left.host == right.host && left.port == right.port &&
           left.clientName == right.clientName && left.clientVersion == right.clientVersion &&
           left.autoReconnect == right.autoReconnect;
}

bool isSerialSourceKey(std::string_view key) {
    return key.starts_with(serialSourcePrefix);
}

bool isNetworkSourceKey(std::string_view key) {
    return key.starts_with(tcpClientSourcePrefix) ||
           key.starts_with(tcpServerSourcePrefix) ||
           key.starts_with(udpSourcePrefix);
}

}  // namespace

SerialSession::SerialSession(QObject* parent) : QObject(parent) {
    sourceManager_.setCallbacks({
        [this](const std::string& key, const lab::core::DataChunk& chunk) {
            if (chunk.direction == lab::core::Direction::Rx) {
                healthAlertRules_.observeActivity(
                    chunk.sourceId, chunk.receiveTimestamp);
            }
            std::scoped_lock routeLock(routingMutex_);
            const auto recording = recorder_.isRecording();
            const auto declared = isDeclaredRecordingSource(key);
            if (declared) {
                recorder_.enqueueRaw(chunk);
            }
            if (key != remoteAgentSourceKey &&
                chunk.direction == lab::core::Direction::Rx &&
                (!recording || declared)) {
                processing_.push(chunk);
            }
            std::scoped_lock lock(uiQueueMutex_);
            uiQueue_.push_back(chunk);
        },
        [this](const std::string& key,
               const std::string& sourceId,
               lab::core::SourceState state) {
            if (state == lab::core::SourceState::Open) {
                healthAlertRules_.arm(sourceId, lab::core::nowTimestampNs());
            }
            const auto label = isSerialSourceKey(key)
                                   ? "Serial"
                                   : isNetworkSourceKey(key) ? "Network" : "Remote Agent";
            if (isDeclaredRecordingSource(key)) {
                recorder_.enqueueEvent({lab::core::nowTimestampNs(),
                                        sourceId,
                                        "info",
                                        "source_state",
                                        std::string(label) + " state changed to " +
                                            std::to_string(static_cast<int>(state)),
                                        0});
            }
            QMetaObject::invokeMethod(
                this,
                [this, key, sourceId, state] {
                    if (key == remoteAgentSourceKey) {
                        if (state == lab::core::SourceState::Open) {
                            const auto now = lab::core::nowTimestampNs();
                            for (const auto& [topic, type] : remoteSubscriptions_) {
                                static_cast<void>(type);
                                healthAlertRules_.arm(
                                    remoteAgent_.topicSourceId(topic), now);
                            }
                        }
                        emit remoteAgentStateChanged(static_cast<int>(state));
                        return;
                    }
                    if (!isLocalSource(key)) {
                        return;
                    }
                    localSourceStates_.insert_or_assign(key, state);
                    const auto type = localSourceType(key);
                    emit localSourceStateChanged(QString::fromStdString(sourceId),
                                                 type,
                                                 static_cast<int>(state));
                    if (key == selectedSerialSource_) {
                        emit sourceStateChanged(static_cast<int>(state));
                    }
                    if (key == selectedNetworkSource_) {
                        emit networkStateChanged(static_cast<int>(state));
                    }
                    publishLocalSources();
                },
                Qt::QueuedConnection);
        },
        [this](const std::string& key,
               const std::string& sourceId,
               const std::string& message) {
            healthAlertRules_.observeError(
                sourceId.empty() ? key : sourceId,
                lab::core::nowTimestampNs());
            if (isDeclaredRecordingSource(key)) {
                recorder_.enqueueEvent({
                    lab::core::nowTimestampNs(),
                    sourceId,
                    "error",
                    key == remoteAgentSourceKey ? "remote_agent" : "source",
                    message,
                    0});
            }
            QMetaObject::invokeMethod(
                this,
                [this, key, sourceId, message] {
                    const auto detail = QString::fromStdString(message);
                    if (key == remoteAgentSourceKey) {
                        emit sourceError(tr("Remote Agent：%1").arg(detail));
                    } else if (isNetworkSourceKey(key)) {
                        emit sourceError(tr("网络 %1：%2")
                                             .arg(QString::fromStdString(sourceId), detail));
                    } else {
                        emit sourceError(tr("串口 %1：%2")
                                             .arg(QString::fromStdString(sourceId), detail));
                    }
                },
                Qt::QueuedConnection);
        },
        [this](const std::string& key, const lab::core::DataSample& sample) {
            if (key != remoteAgentSourceKey) {
                return;
            }
            {
                std::scoped_lock routeLock(routingMutex_);
                const auto declared = isDeclaredRecordingSource(key);
                timeSeries_.append(sample);
                updateLatestTimestamp(latestLiveTimestamp_, sample.timestamp);
                if (declared) {
                    recorder_.enqueueSample(sample);
                }
            }
            discoverLiveField(sample.field);
        },
        [this](const std::string& key,
               std::span<const lab::core::DataSample> samples) {
            if (key != remoteAgentSourceKey || samples.empty()) return;
            healthAlertRules_.observeActivity(
                samples.front().sourceId, lab::core::nowTimestampNs());
            std::vector<lab::core::DataSample> derived;
            {
                std::scoped_lock routeLock(routingMutex_);
                const auto recording = recorder_.isRecording();
                const auto declared = isDeclaredRecordingSource(key);
                if (!recording || declared) {
                    processAlerts(samples, declared);
                    derived = appendDerivedSamples(samples, declared);
                }
            }
            for (const auto& output : derived) discoverLiveField(output.field);
        }});
    static_cast<void>(sourceManager_.add(std::string(remoteAgentSourceKey), remoteAgent_));

    remoteAgent_.setAgentCallbacks({
        [this](const lab::core::agent::Hello& hello) {
            const auto agentId = QString::fromStdString(hello.agentId);
            const auto version = QString::fromStdString(hello.softwareVersion);
            const auto hostName = QString::fromStdString(hello.hostName);
            QMetaObject::invokeMethod(
                this,
                [this, agentId, version, hostName, capabilities = hello.capabilities] {
                    emit remoteAgentHello(agentId, version, hostName, capabilities);
                },
                Qt::QueuedConnection);
        },
        [this](const lab::core::agent::TopicCatalog& catalog) {
            QVariantList topics;
            topics.reserve(static_cast<qsizetype>(catalog.topics.size()));
            for (const auto& topic : catalog.topics) {
                QVariantMap item;
                item.insert(QStringLiteral("name"), QString::fromStdString(topic.name));
                item.insert(QStringLiteral("type"), QString::fromStdString(topic.type));
                item.insert(QStringLiteral("reliability"),
                            static_cast<int>(topic.reliability));
                item.insert(QStringLiteral("durability"),
                            static_cast<int>(topic.durability));
                topics.push_back(item);
            }
            QMetaObject::invokeMethod(
                this,
                [this, topics, revision = catalog.graphRevision] {
                    emit remoteTopicsChanged(topics, revision);
                },
                Qt::QueuedConnection);
        },
        [this](const lab::core::agent::SampleBatch& batch) {
            const auto now = lab::core::nowTimestampNs();
            healthAlertRules_.observeActivity(remoteAgent_.sourceId(), now);
            healthAlertRules_.observeActivity(
                remoteAgent_.topicSourceId(batch.topic), now);
        },
        {},
        [this](const lab::core::ClockSyncEstimate& estimate) {
            if (isDeclaredRecordingSource(std::string(remoteAgentSourceKey))) {
                recorder_.enqueueEvent({
                    estimate.measuredAtNs,
                    remoteAgent_.sourceId(),
                    "info",
                    "clock_sync",
                    "offset_ns=" + std::to_string(estimate.offsetNs) +
                        ";round_trip_ns=" + std::to_string(estimate.roundTripNs) +
                        ";uncertainty_ns=" + std::to_string(estimate.uncertaintyNs) +
                        ";samples=" + std::to_string(estimate.sampleCount),
                    0});
            }
            QMetaObject::invokeMethod(
                this,
                [this, estimate] {
                    emit remoteAgentClockSync(
                        estimate.offsetNs,
                        estimate.roundTripNs,
                        estimate.uncertaintyNs,
                        static_cast<quint32>(estimate.sampleCount));
                },
                Qt::QueuedConnection);
        },
        [this](const lab::core::agent::TopicFieldCatalog& catalog) {
            QVariantList topics;
            topics.reserve(static_cast<qsizetype>(catalog.topics.size()));
            for (const auto& topic : catalog.topics) {
                QVariantMap item;
                item.insert(QStringLiteral("name"), QString::fromStdString(topic.name));
                item.insert(QStringLiteral("type"), QString::fromStdString(topic.type));
                item.insert(QStringLiteral("mapping"),
                            static_cast<int>(topic.mapping));
                item.insert(QStringLiteral("reason"),
                            QString::fromStdString(topic.reason));
                topics.push_back(item);
            }
            QMetaObject::invokeMethod(
                this,
                [this, topics, revision = catalog.graphRevision] {
                    emit remoteTopicFieldsChanged(topics, revision);
                },
                Qt::QueuedConnection);
        }});

    replay_.setCallbacks({
        [this](const lab::core::DataChunk& chunk) {
            QStringList discoveredFields;
            std::string mappingWarning;
            {
                std::scoped_lock routeLock(routingMutex_);
                if (replayStructuredRosbag_.load() &&
                    chunk.direction == lab::core::Direction::Rx) {
                    const auto topic = rosbagReplayTopics_.find(chunk.sourceId);
                    if (topic != rosbagReplayTopics_.end()) {
                        const auto mapping =
                            lab::adapters::rosbag2::mapStructuredCdrFields(
                                topic->second.second, chunk.payload);
                        if (mapping.success) {
                            const auto timestamp = mapping.sourceTimestamp != 0
                                                       ? mapping.sourceTimestamp
                                                       : chunk.sourceTimestamp;
                            bool changed = false;
                            std::vector<lab::core::DataSample> mappedSamples;
                            mappedSamples.reserve(mapping.fields.size());
                            for (const auto& field : mapping.fields) {
                                const auto name = topic->second.first + "." + field.path;
                                mappedSamples.push_back({timestamp,
                                                         chunk.sourceId,
                                                         name,
                                                         field.value,
                                                         field.unit,
                                                         chunk.sequence});
                                timeSeries_.append(mappedSamples.back());
                                changed = replayFieldNames_.insert(name).second || changed;
                            }
                            for (const auto& output :
                                 appendDerivedSamples(mappedSamples, false)) {
                                changed = replayFieldNames_.insert(output.field).second || changed;
                            }
                            if (changed) {
                                for (const auto& field : replayFieldNames_) {
                                    discoveredFields.push_back(QString::fromStdString(field));
                                }
                            }
                        } else if (mapping.supported &&
                                   replayMappingWarnings_.insert(chunk.sourceId).second) {
                            mappingWarning = mapping.warning;
                        }
                    }
                } else if (!replayRawOnly_.load() &&
                           chunk.direction == lab::core::Direction::Rx) {
                    processing_.push(chunk);
                }
            }
            if (!discoveredFields.isEmpty()) {
                QMetaObject::invokeMethod(
                    this,
                    [this, discoveredFields] {
                        emit replayFieldsDiscovered(discoveredFields);
                    },
                    Qt::QueuedConnection);
            }
            if (!mappingWarning.empty()) {
                QMetaObject::invokeMethod(
                    this,
                    [this, mappingWarning] {
                        emit sourceError(
                            tr("rosbag2 结构化解析已回退为原始 CDR：%1")
                                .arg(QString::fromStdString(mappingWarning)));
                    },
                    Qt::QueuedConnection);
            }
            {
                std::scoped_lock lock(uiQueueMutex_);
                uiQueue_.push_back(chunk);
            }
        },
        {},
        [this](const std::string& message) {
            QMetaObject::invokeMethod(
                this,
                [this, message] {
                    emit sourceError(tr("回放：%1").arg(QString::fromStdString(message)));
                },
                Qt::QueuedConnection);
        },
        {},
        {}});

    processing_.setQualifyFieldNames(true);
    processing_.setSampleHandler([this](const lab::core::DataSample& sample) {
        recorder_.enqueueSample(sample);
        updateLatestTimestamp(latestLiveTimestamp_, sample.timestamp);
        discoverLiveField(sample.field);
    });
    processing_.setSampleBatchHandler(
        [this](std::span<const lab::core::DataSample> samples) {
            processAlerts(samples, true);
            for (const auto& output : appendDerivedSamples(samples, true)) {
                discoverLiveField(output.field);
            }
        });

    processing_.setFrameHandler([this](const lab::core::FrameEvent& event) {
        {
            std::scoped_lock lock(protocolQueueMutex_);
            protocolQueue_.push_back(event);
        }
        if (event.kind != lab::core::FrameEventKind::FrameDecoded) {
            healthAlertRules_.observeError(
                event.sourceId, lab::core::nowTimestampNs());
            recorder_.enqueueEvent({event.sourceTimestamp,
                                    event.sourceId,
                                    "warning",
                                    "protocol",
                                    event.message,
                                    event.sequence});
        } else {
            recorder_.enqueueFrame(event);
        }
    });

    refreshTimer_.setInterval(33);
    refreshTimer_.setTimerType(Qt::PreciseTimer);
    connect(&refreshTimer_, &QTimer::timeout, this, &SerialSession::drainUiQueue);
    refreshTimer_.start();
}

SerialSession::~SerialSession() {
    rosbagImporting_.store(false);
    if (rosbagImportWorker_.joinable()) {
        rosbagImportWorker_.request_stop();
        rosbagImportWorker_.join();
    }
    sourceManager_.closeAll();
    sourceManager_.setCallbacks({});
    remoteAgent_.setAgentCallbacks({});
    replay_.close();
    processing_.flush();
    recorder_.stop();
}

std::vector<lab::adapters::serial::PortInfo> SerialSession::availablePorts() {
    return lab::adapters::serial::SerialSource::availablePorts();
}

const lab::core::TimeSeriesStore& SerialSession::timeSeries() const noexcept {
    return timeSeries_;
}

void SerialSession::discoverLiveField(const std::string& field) {
    QStringList fields;
    {
        std::scoped_lock lock(liveFieldsMutex_);
        if (!liveFieldNames_.insert(field).second) {
            return;
        }
        fields.reserve(static_cast<qsizetype>(liveFieldNames_.size()));
        for (const auto& name : liveFieldNames_) {
            fields.push_back(QString::fromStdString(name));
        }
    }
    QMetaObject::invokeMethod(
        this,
        [this, fields] { emit liveFieldsDiscovered(fields); },
        Qt::QueuedConnection);
}

bool SerialSession::isDeclaredRecordingSource(const std::string& key) const noexcept {
    std::scoped_lock lock(recordingSourcesMutex_);
    return recordingSourceKeys_.contains(key);
}

bool SerialSession::isLocalSource(const std::string& key) const noexcept {
    return serialSources_.contains(key) || networkSources_.contains(key);
}

QString SerialSession::localSourceType(const std::string& key) const {
    return serialSources_.contains(key) ? QStringLiteral("serial")
         : networkSources_.contains(key) ? QStringLiteral("network")
                                        : QString{};
}

std::vector<lab::core::DataSample> SerialSession::appendDerivedSamples(
    std::span<const lab::core::DataSample> inputs,
    bool record) {
    auto output = derivedFields_.consumeBatch(inputs);
    for (const auto& sample : output) {
        timeSeries_.append(sample);
        if (record) recorder_.enqueueSample(sample);
    }
    processAlerts(output, record);
    return output;
}

void SerialSession::processAlerts(
    std::span<const lab::core::DataSample> samples,
    bool record) {
    if (samples.empty() || replaying_.load()) return;
    for (const auto& trigger : alertRules_.consumeBatch(samples)) {
        std::ostringstream detail;
        detail.precision(17);
        detail << '[' << trigger.name << "] " << trigger.field << '='
               << trigger.value << ' '
               << (trigger.comparison == lab::core::ThresholdComparison::Above
                       ? '>'
                       : '<')
               << ' ' << trigger.threshold;
        if (trigger.durationNs > 0) {
            detail << " sustained_for_ms=" << trigger.durationNs / 1'000'000;
        }
        if (!trigger.message.empty()) detail << " — " << trigger.message;
        publishTimelineEvent({trigger.timestamp,
                              trigger.sourceId,
                              "warning",
                              "alert",
                              detail.str(),
                              trigger.sequence},
                             record);
    }
}

bool SerialSession::shouldRecordHealthAlert(
    const std::string& sourceId) const noexcept {
    if (std::string_view(sourceId).starts_with("ros-agent:")) {
        return isDeclaredRecordingSource(std::string(remoteAgentSourceKey));
    }
    return isDeclaredRecordingSource(sourceId);
}

void SerialSession::armHealthAlertTargets() {
    const auto now = lab::core::nowTimestampNs();
    for (const auto& [sourceId, source] : serialSources_) {
        if (source->isOpen()) healthAlertRules_.arm(sourceId, now);
    }
    for (const auto& [sourceId, source] : networkSources_) {
        if (source->isOpen()) healthAlertRules_.arm(sourceId, now);
    }
    if (remoteAgent_.isOpen()) {
        healthAlertRules_.arm(remoteAgent_.sourceId(), now);
        for (const auto& [topic, type] : remoteSubscriptions_) {
            static_cast<void>(type);
            healthAlertRules_.arm(remoteAgent_.topicSourceId(topic), now);
        }
    }
}

void SerialSession::processHealthAlerts() {
    if (replaying_.load()) return;
    for (const auto& trigger :
         healthAlertRules_.evaluate(lab::core::nowTimestampNs())) {
        std::ostringstream detail;
        detail << '[' << trigger.name << "] " << trigger.sourceId;
        if (trigger.kind == lab::core::HealthAlertKind::ErrorRate) {
            detail << " errors=" << trigger.observedErrors
                   << " threshold=" << trigger.configuredErrors
                   << " window_ms=" << trigger.windowNs / 1'000'000;
        } else {
            detail << " no_data_for_ms=" << trigger.windowNs / 1'000'000;
        }
        if (!trigger.message.empty()) detail << " — " << trigger.message;
        const auto record = recorder_.isRecording() &&
                            shouldRecordHealthAlert(trigger.sourceId);
        publishTimelineEvent({trigger.timestamp,
                              trigger.sourceId,
                              "warning",
                              "alert",
                              detail.str(),
                              trigger.sequence},
                             record);
    }
}

void SerialSession::publishTimelineEvent(
    lab::core::SessionEvent event,
    bool record) {
    event.sequence = nextTimelineSequence_.fetch_add(1);
    QVariantList snapshot;
    {
        std::scoped_lock lock(timelineEventsMutex_);
        if (timelineEvents_.size() ==
            static_cast<std::size_t>(maximumTimelineEvents)) {
            timelineEvents_.pop_front();
        }
        timelineEvents_.push_back(event);
        snapshot.reserve(static_cast<qsizetype>(timelineEvents_.size()));
        for (const auto& item : timelineEvents_) {
            snapshot.push_back(eventToVariant(item));
        }
    }
    if (record) recorder_.enqueueEvent(std::move(event));
    emit timelineEventsChanged(snapshot);
}

void SerialSession::clearTimelineEvents() {
    {
        std::scoped_lock lock(timelineEventsMutex_);
        timelineEvents_.clear();
    }
    emit timelineEventsChanged({});
}

void SerialSession::leaveReplayForLiveSource() {
    replay_.close();
    if (!replaying_.exchange(false)) return;
    replayRawOnly_.store(false);
    replayStructuredRosbag_.store(false);
    {
        std::scoped_lock routeLock(routingMutex_);
        rosbagReplayTopics_.clear();
        replayFieldNames_.clear();
        replayMappingWarnings_.clear();
    }
    alertRules_.resetValues();
    healthAlertRules_.resetValues();
    armHealthAlertTargets();
    latestLiveTimestamp_.store(0);
    nextTimelineSequence_.store(0);
    clearTimelineEvents();
}

void SerialSession::publishLocalSources() {
    QVariantList sources;
    sources.reserve(static_cast<qsizetype>(serialSources_.size() +
                                           networkSources_.size()));
    for (const auto& [sourceId, source] : serialSources_) {
        const auto settings = source->settings();
        QVariantMap item;
        item.insert(QStringLiteral("id"), QString::fromStdString(sourceId));
        item.insert(QStringLiteral("type"), QStringLiteral("serial"));
        item.insert(QStringLiteral("selected"), sourceId == selectedSerialSource_);
        item.insert(QStringLiteral("state"), static_cast<int>(
            localSourceStates_.contains(sourceId)
                ? localSourceStates_.at(sourceId)
                : lab::core::SourceState::Closed));
        item.insert(QStringLiteral("open"), source->isOpen());
        item.insert(QStringLiteral("port"), QString::fromStdString(settings.portName));
        item.insert(QStringLiteral("baud_rate"), settings.baudRate);
        item.insert(QStringLiteral("data_bits"), settings.dataBits);
        item.insert(QStringLiteral("stop_bits"), static_cast<int>(settings.stopBits));
        item.insert(QStringLiteral("parity"), static_cast<int>(settings.parity));
        item.insert(QStringLiteral("flow_control"),
                    static_cast<int>(settings.flowControl));
        sources.push_back(item);
    }
    for (const auto& [sourceId, source] : networkSources_) {
        const auto settings = source->settings();
        QVariantMap item;
        item.insert(QStringLiteral("id"), QString::fromStdString(sourceId));
        item.insert(QStringLiteral("type"), QStringLiteral("network"));
        item.insert(QStringLiteral("selected"), sourceId == selectedNetworkSource_);
        item.insert(QStringLiteral("state"), static_cast<int>(
            localSourceStates_.contains(sourceId)
                ? localSourceStates_.at(sourceId)
                : lab::core::SourceState::Closed));
        item.insert(QStringLiteral("open"), source->isOpen());
        item.insert(QStringLiteral("mode"), static_cast<int>(settings.mode));
        item.insert(QStringLiteral("remote_host"),
                    QString::fromStdString(settings.remoteHost));
        item.insert(QStringLiteral("remote_port"), settings.remotePort);
        item.insert(QStringLiteral("bind_address"),
                    QString::fromStdString(settings.bindAddress));
        item.insert(QStringLiteral("local_port"), settings.localPort);
        sources.push_back(item);
    }
    emit localSourcesChanged(sources);
}

void SerialSession::publishParserSources() {
    std::set<std::string> sourceIds;
    for (const auto& [sourceId, source] : serialSources_) {
        static_cast<void>(source);
        sourceIds.insert(sourceId);
    }
    for (const auto& [sourceId, source] : networkSources_) {
        static_cast<void>(source);
        sourceIds.insert(sourceId);
    }
    for (const auto& [sourceId, configuration] : sourceParserConfigurations_) {
        static_cast<void>(configuration);
        sourceIds.insert(sourceId);
    }
    QVariantList sources;
    sources.reserve(static_cast<qsizetype>(sourceIds.size()));
    for (const auto& sourceId : sourceIds) {
        QVariantMap source;
        source.insert(QStringLiteral("id"), QString::fromStdString(sourceId));
        source.insert(QStringLiteral("label"), QString::fromStdString(sourceId));
        const auto configured = sourceParserConfigurations_.find(sourceId);
        source.insert(QStringLiteral("overridden"),
                      configured != sourceParserConfigurations_.end());
        source.insert(
            QStringLiteral("mode"),
            configured == sourceParserConfigurations_.end()
                ? QStringLiteral("default")
                : configured->second.protocolDefinition
                      ? QStringLiteral("protocol")
                      : QStringLiteral("csv"));
        sources.push_back(source);
    }
    emit parserSourcesChanged(sources);
}

void SerialSession::clearSourceParserConfigurations() {
    for (const auto& [sourceId, configuration] : sourceParserConfigurations_) {
        static_cast<void>(configuration);
        processing_.clearSourceConfiguration(sourceId);
    }
    sourceParserConfigurations_.clear();
    publishParserSources();
}

lab::core::SessionParserConfiguration
SerialSession::resolvedParserConfiguration(const std::string& sourceId) const {
    const auto configured = sourceParserConfigurations_.find(sourceId);
    if (configured != sourceParserConfigurations_.end()) {
        return {configured->second.csvFields,
                configured->second.protocolName,
                configured->second.protocolJson};
    }
    return {activeCsvFields_, activeProtocolName_, activeProtocolJson_};
}

void SerialSession::connectSerial(lab::adapters::serial::SerialSettings settings) {
    const auto key = lab::adapters::serial::serialSourceId(settings);
    if (settings.portName.empty()) {
        emit sourceError(tr("串口名称不能为空"));
        return;
    }
    const auto existing = serialSources_.find(key);
    if (recorder_.isRecording() &&
        (existing == serialSources_.end() ||
         !isDeclaredRecordingSource(key) ||
         !sameSettings(settings, existing->second->settings()))) {
        emit sourceError(tr("当前 Session 未声明此串口配置，停止记录后才能新增或更改"));
        return;
    }
    if (existing == serialSources_.end() &&
        serialSources_.size() + networkSources_.size() >= maximumLocalSources) {
        emit sourceError(tr("本地数据源不能超过 16 个"));
        return;
    }

    leaveReplayForLiveSource();
    selectedSerialSource_ = key;
    sendTargetSource_ = key;
    if (existing == serialSources_.end()) {
        auto source = std::make_unique<lab::adapters::serial::SerialSource>();
        source->setSettings(settings);
        if (!sourceManager_.add(key, *source)) {
            emit sourceError(tr("无法登记串口数据源：%1")
                                 .arg(QString::fromStdString(key)));
            return;
        }
        serialSources_.emplace(key, std::move(source));
        localSourceStates_.insert_or_assign(key, lab::core::SourceState::Closed);
    } else {
        sourceManager_.close(key);
        existing->second->setSettings(settings);
    }
    publishLocalSources();
    publishParserSources();
    if (!sourceManager_.open(key)) {
        publishLocalSources();
    }
}

void SerialSession::disconnectSerial() {
    if (!selectedSerialSource_.empty()) {
        disconnectLocalSource(QString::fromStdString(selectedSerialSource_));
    }
}

void SerialSession::reconnectSerial() {
    if (selectedSerialSource_.empty()) {
        emit sourceError(tr("请先选择并连接一次目标串口"));
        return;
    }
    reconnectLocalSource(QString::fromStdString(selectedSerialSource_));
}

void SerialSession::connectNetwork(lab::adapters::network::NetworkSettings settings) {
    const auto key = lab::adapters::network::networkSourceId(settings);
    const auto existing = networkSources_.find(key);
    if (recorder_.isRecording() &&
        (existing == networkSources_.end() ||
         !isDeclaredRecordingSource(key) ||
         !sameSettings(settings, existing->second->settings()))) {
        emit sourceError(tr("当前 Session 未声明此网络配置，停止记录后才能新增或更改"));
        return;
    }
    if (existing == networkSources_.end() &&
        serialSources_.size() + networkSources_.size() >= maximumLocalSources) {
        emit sourceError(tr("本地数据源不能超过 16 个"));
        return;
    }

    leaveReplayForLiveSource();
    selectedNetworkSource_ = key;
    sendTargetSource_ = key;
    if (existing == networkSources_.end()) {
        auto source = std::make_unique<lab::adapters::network::NetworkSource>();
        source->setSettings(settings);
        if (!sourceManager_.add(key, *source)) {
            emit sourceError(tr("无法登记网络数据源：%1")
                                 .arg(QString::fromStdString(key)));
            return;
        }
        networkSources_.emplace(key, std::move(source));
        localSourceStates_.insert_or_assign(key, lab::core::SourceState::Closed);
    } else {
        sourceManager_.close(key);
        existing->second->setSettings(settings);
    }
    publishLocalSources();
    publishParserSources();
    if (!sourceManager_.open(key)) {
        publishLocalSources();
    }
}

void SerialSession::disconnectNetwork() {
    if (!selectedNetworkSource_.empty()) {
        disconnectLocalSource(QString::fromStdString(selectedNetworkSource_));
    }
}

void SerialSession::reconnectNetwork() {
    if (selectedNetworkSource_.empty()) {
        emit sourceError(tr("请先配置并打开一次网络数据源"));
        return;
    }
    reconnectLocalSource(QString::fromStdString(selectedNetworkSource_));
}

void SerialSession::disconnectLocalSource(const QString& sourceId) {
    const auto key = utf8String(sourceId.trimmed());
    if (!isLocalSource(key)) {
        emit sourceError(tr("找不到本地数据源：%1").arg(sourceId));
        return;
    }
    sourceManager_.close(key);
    publishLocalSources();
}

void SerialSession::reconnectLocalSource(const QString& sourceId) {
    const auto key = utf8String(sourceId.trimmed());
    if (!isLocalSource(key)) {
        emit sourceError(tr("找不到本地数据源：%1").arg(sourceId));
        return;
    }
    if (recorder_.isRecording() && !isDeclaredRecordingSource(key)) {
        emit sourceError(tr("当前 Session 未声明此数据源，停止记录后才能加入"));
        return;
    }
    leaveReplayForLiveSource();
    sourceManager_.close(key);
    sendTargetSource_ = key;
    if (serialSources_.contains(key)) {
        selectedSerialSource_ = key;
    } else {
        selectedNetworkSource_ = key;
    }
    if (!sourceManager_.open(key)) {
        publishLocalSources();
    }
}

bool SerialSession::removeLocalSource(const QString& sourceId) {
    if (recorder_.isRecording()) {
        emit sourceError(tr("Session 记录期间不能移除数据源；可先断开，停止记录后再移除"));
        return false;
    }
    const auto key = utf8String(sourceId.trimmed());
    if (!isLocalSource(key)) {
        emit sourceError(tr("找不到本地数据源：%1").arg(sourceId));
        return false;
    }
    if (!sourceManager_.remove(key)) {
        emit sourceError(tr("无法移除本地数据源：%1").arg(sourceId));
        return false;
    }
    serialSources_.erase(key);
    networkSources_.erase(key);
    localSourceStates_.erase(key);
    processing_.clearSourceConfiguration(key);
    sourceParserConfigurations_.erase(key);
    healthAlertRules_.disarm(key);
    if (selectedSerialSource_ == key) {
        selectedSerialSource_ = serialSources_.empty()
                                    ? std::string{}
                                    : serialSources_.begin()->first;
    }
    if (selectedNetworkSource_ == key) {
        selectedNetworkSource_ = networkSources_.empty()
                                     ? std::string{}
                                     : networkSources_.begin()->first;
    }
    if (sendTargetSource_ == key) {
        selectFallbackSendTarget();
    }
    publishLocalSources();
    publishParserSources();
    return true;
}

void SerialSession::disconnectAllLocalSources() {
    for (const auto& [sourceId, source] : serialSources_) {
        static_cast<void>(source);
        sourceManager_.close(sourceId);
    }
    for (const auto& [sourceId, source] : networkSources_) {
        static_cast<void>(source);
        sourceManager_.close(sourceId);
    }
    publishLocalSources();
}

void SerialSession::connectRemoteAgent(
    lab::adapters::remote_agent::RemoteAgentSettings settings) {
    if (recorder_.isRecording() &&
        (!isDeclaredRecordingSource(std::string(remoteAgentSourceKey)) ||
         !sameSettings(settings, lastRemoteAgentSettings_))) {
        emit sourceError(tr("当前 Session 未声明此 Remote Agent 配置，停止记录后才能更改"));
        return;
    }
    leaveReplayForLiveSource();
    if (remoteAgentConfigured_ &&
        (settings.host != lastRemoteAgentSettings_.host ||
         settings.port != lastRemoteAgentSettings_.port)) {
        for (const auto& [topic, type] : remoteSubscriptions_) {
            static_cast<void>(type);
            healthAlertRules_.disarm(remoteAgent_.topicSourceId(topic));
        }
    }
    sourceManager_.close(std::string(remoteAgentSourceKey));
    lastRemoteAgentSettings_ = std::move(settings);
    remoteAgentConfigured_ = true;
    remoteAgent_.setSettings(lastRemoteAgentSettings_);
    sourceManager_.open(std::string(remoteAgentSourceKey));
}

void SerialSession::disconnectRemoteAgent() {
    sourceManager_.close(std::string(remoteAgentSourceKey));
}

void SerialSession::reconnectRemoteAgent() {
    if (!remoteAgentConfigured_) {
        emit sourceError(tr("请先配置并连接一次 Remote Agent"));
        return;
    }
    if (recorder_.isRecording() &&
        !isDeclaredRecordingSource(std::string(remoteAgentSourceKey))) {
        emit sourceError(tr("当前 Session 未声明 Remote Agent，停止记录后才能加入"));
        return;
    }
    leaveReplayForLiveSource();
    sourceManager_.close(std::string(remoteAgentSourceKey));
    remoteAgent_.setSettings(lastRemoteAgentSettings_);
    sourceManager_.open(std::string(remoteAgentSourceKey));
}

void SerialSession::setSendTarget(int target) {
    if (target == 0) {
        if (!selectedSerialSource_.empty()) {
            sendTargetSource_ = selectedSerialSource_;
        } else if (!serialSources_.empty()) {
            sendTargetSource_ = serialSources_.begin()->first;
        }
    } else if (target == 1) {
        if (!selectedNetworkSource_.empty()) {
            sendTargetSource_ = selectedNetworkSource_;
        } else if (!networkSources_.empty()) {
            sendTargetSource_ = networkSources_.begin()->first;
        }
    }
}

void SerialSession::setSendTargetSource(const QString& sourceId) {
    const auto key = utf8String(sourceId.trimmed());
    if (key.empty()) {
        sendTargetSource_.clear();
        return;
    }
    if (!isLocalSource(key)) {
        emit sourceError(tr("发送目标不存在：%1").arg(sourceId));
        return;
    }
    sendTargetSource_ = key;
}

void SerialSession::selectFallbackSendTarget() {
    sendTargetSource_.clear();
    for (const auto& [key, source] : serialSources_) {
        if (source->isOpen()) {
            sendTargetSource_ = key;
            return;
        }
    }
    for (const auto& [key, source] : networkSources_) {
        if (source->isOpen()) {
            sendTargetSource_ = key;
            return;
        }
    }
}

void SerialSession::requestRemoteTopics() {
    if (!remoteAgent_.requestTopicCatalog()) {
        emit sourceError(tr("Remote Agent 尚未完成握手，无法刷新 Topic"));
    }
}

void SerialSession::subscribeRemoteTopic(
    lab::core::agent::SubscriptionRequest request) {
    if (!remoteAgent_.subscribe(request)) {
        emit sourceError(tr("Remote Agent 订阅请求未被发送"));
        return;
    }
    remoteSubscriptions_.insert({request.topic, request.type});
    healthAlertRules_.arm(
        remoteAgent_.topicSourceId(request.topic),
        lab::core::nowTimestampNs());
}

void SerialSession::unsubscribeRemoteTopic(
    lab::core::agent::SubscriptionRequest request) {
    if (!remoteAgent_.unsubscribe(request)) {
        emit sourceError(tr("Remote Agent 取消订阅请求未被发送"));
        return;
    }
    remoteSubscriptions_.erase({request.topic, request.type});
    const auto sameTopicRemains = std::ranges::any_of(
        remoteSubscriptions_, [&request](const auto& subscription) {
            return subscription.first == request.topic;
        });
    if (!sameTopicRemains) {
        healthAlertRules_.disarm(
            remoteAgent_.topicSourceId(request.topic));
    }
}

void SerialSession::sendBytes(const QByteArray& bytes) {
    const auto first = reinterpret_cast<const std::uint8_t*>(bytes.constData());
    const auto data = std::span(first, static_cast<std::size_t>(bytes.size()));
    const auto accepted = !sendTargetSource_.empty() &&
                          sourceManager_.write(sendTargetSource_, data);
    if (!accepted) {
        emit sourceError(tr("发送失败：当前数据源未连接或写入未被接受"));
    }
}

void SerialSession::setCsvFields(const QStringList& fields) {
    if (recorder_.isRecording()) {
        emit sourceError(tr("Session 记录期间解析配置已冻结，请停止记录后再修改"));
        return;
    }
    QStringList issues;
    auto names = validatedCsvFields(fields, issues);
    if (!names) {
        emit sourceParserConfigurationFailed({}, issues);
        return;
    }
    activeCsvFields_ = *names;
    processing_.setFieldNames(std::move(*names));
    resetParserDependentState();
    if (activeProtocolName_.empty()) {
        emit sourceParserConfigured({}, QStringLiteral("csv"), {}, fields);
    }
}

bool SerialSession::setSourceCsvFields(const QString& sourceId,
                                       const QStringList& fields) {
    if (recorder_.isRecording()) {
        emit sourceParserConfigurationFailed(
            sourceId,
            {tr("Session 记录期间逐来源解析配置已冻结，请停止记录后再修改")});
        return false;
    }
    const auto cleanedSource = sourceId.trimmed();
    const auto sourceBytes = cleanedSource.toUtf8();
    QStringList issues;
    if (sourceBytes.isEmpty() || sourceBytes.size() > 1024 ||
        containsControlCharacter(cleanedSource)) {
        issues.push_back(tr("数据源标识为空、过长或包含控制字符"));
    }
    auto names = validatedCsvFields(fields, issues);
    if (!names) {
        emit sourceParserConfigurationFailed(sourceId, issues);
        return false;
    }
    const auto key = utf8String(cleanedSource);
    if (!sourceParserConfigurations_.contains(key) &&
        sourceParserConfigurations_.size() >=
            static_cast<std::size_t>(maximumParserSources)) {
        emit sourceParserConfigurationFailed(
            sourceId, {tr("逐来源解析配置不能超过 64 项")});
        return false;
    }
    lab::core::ProcessingPipeline::ParserConfiguration pipelineConfiguration;
    pipelineConfiguration.fieldNames = *names;
    if (!processing_.setSourceConfiguration(key,
                                            std::move(pipelineConfiguration))) {
        emit sourceParserConfigurationFailed(sourceId, {tr("数据源标识无效")});
        return false;
    }
    sourceParserConfigurations_.insert_or_assign(
        key, ActiveParserConfiguration{*names, std::nullopt, {}, {}});
    resetParserDependentState();
    publishParserSources();
    emit sourceParserConfigured(cleanedSource,
                                QStringLiteral("csv"),
                                {},
                                fields);
    return true;
}

bool SerialSession::loadSourceProtocolFile(const QString& sourceId,
                                           const QString& path) {
    if (recorder_.isRecording()) {
        emit sourceParserConfigurationFailed(
            sourceId,
            {tr("Session 记录期间逐来源解析配置已冻结，请停止记录后再修改")});
        return false;
    }
    const auto cleanedSource = sourceId.trimmed();
    const auto sourceBytes = cleanedSource.toUtf8();
    if (sourceBytes.isEmpty() || sourceBytes.size() > 1024 ||
        containsControlCharacter(cleanedSource)) {
        emit sourceParserConfigurationFailed(
            sourceId, {tr("数据源标识为空、过长或包含控制字符")});
        return false;
    }
    QStringList issues;
    auto loaded = readProtocolDefinition(path, issues);
    if (!loaded) {
        emit sourceParserConfigurationFailed(sourceId, issues);
        return false;
    }
    const auto key = utf8String(cleanedSource);
    if (!sourceParserConfigurations_.contains(key) &&
        sourceParserConfigurations_.size() >=
            static_cast<std::size_t>(maximumParserSources)) {
        emit sourceParserConfigurationFailed(
            sourceId, {tr("逐来源解析配置不能超过 64 项")});
        return false;
    }
    auto fieldNames = activeCsvFields_;
    if (const auto previous = sourceParserConfigurations_.find(key);
        previous != sourceParserConfigurations_.end()) {
        fieldNames = previous->second.csvFields;
    }
    lab::core::ProcessingPipeline::ParserConfiguration pipelineConfiguration;
    pipelineConfiguration.fieldNames = fieldNames;
    pipelineConfiguration.protocolDefinition = loaded->definition;
    if (!processing_.setSourceConfiguration(key,
                                            std::move(pipelineConfiguration))) {
        emit sourceParserConfigurationFailed(sourceId, {tr("数据源标识无效")});
        return false;
    }
    const auto protocolName = loaded->definition.name;
    sourceParserConfigurations_.insert_or_assign(
        key,
        ActiveParserConfiguration{fieldNames,
                                  std::move(loaded->definition),
                                  protocolName,
                                  std::move(loaded->json)});
    resetParserDependentState();
    publishParserSources();
    emit sourceParserConfigured(cleanedSource,
                                QStringLiteral("protocol"),
                                QString::fromStdString(protocolName),
                                loaded->numericFields);
    return true;
}

bool SerialSession::clearSourceProtocol(const QString& sourceId) {
    const auto cleanedSource = sourceId.trimmed();
    const auto key = utf8String(cleanedSource);
    const auto found = sourceParserConfigurations_.find(key);
    const auto fields = found == sourceParserConfigurations_.end()
                            ? activeCsvFields_
                            : found->second.csvFields;
    QStringList qtFields;
    qtFields.reserve(static_cast<qsizetype>(fields.size()));
    for (const auto& field : fields) {
        qtFields.push_back(QString::fromStdString(field));
    }
    return setSourceCsvFields(cleanedSource, qtFields);
}

bool SerialSession::resetSourceParserConfiguration(const QString& sourceId) {
    if (recorder_.isRecording()) {
        emit sourceParserConfigurationFailed(
            sourceId,
            {tr("Session 记录期间逐来源解析配置已冻结，请停止记录后再修改")});
        return false;
    }
    const auto cleanedSource = sourceId.trimmed();
    const auto key = utf8String(cleanedSource);
    if (key.empty()) {
        emit sourceParserConfigurationFailed(sourceId, {tr("数据源标识不能为空")});
        return false;
    }
    processing_.clearSourceConfiguration(key);
    sourceParserConfigurations_.erase(key);
    resetParserDependentState();
    publishParserSources();
    QStringList fields;
    for (const auto& field : activeCsvFields_) {
        fields.push_back(QString::fromStdString(field));
    }
    emit sourceParserConfigured(cleanedSource,
                                QStringLiteral("default"),
                                QString::fromStdString(activeProtocolName_),
                                fields);
    return true;
}

void SerialSession::resetParserDependentState() {
    derivedFields_.resetValues();
    alertRules_.resetValues();
    healthAlertRules_.resetValues();
    armHealthAlertTargets();
    latestLiveTimestamp_.store(0);
    timeSeries_.clear();
    {
        std::scoped_lock lock(liveFieldsMutex_);
        liveFieldNames_.clear();
    }
    {
        std::scoped_lock lock(protocolQueueMutex_);
        protocolQueue_.clear();
    }
}

bool SerialSession::setDerivedFields(const QVariantList& definitions) {
    if (recorder_.isRecording()) {
        const QStringList messages{
            tr("Session 记录期间派生变量配置已冻结，请停止记录后再修改")};
        emit derivedFieldsConfigured(false, messages);
        return false;
    }

    std::vector<lab::core::DerivedFieldDefinition> requested;
    requested.reserve(static_cast<std::size_t>(definitions.size()));
    for (const auto& value : definitions) {
        const auto definition = value.toMap();
        const auto name = definition.value(QStringLiteral("name")).toString().trimmed().toUtf8();
        const auto expression =
            definition.value(QStringLiteral("expression")).toString().trimmed().toUtf8();
        const auto unit = definition.value(QStringLiteral("unit")).toString().trimmed().toUtf8();
        requested.push_back({
            std::string(name.constData(), static_cast<std::size_t>(name.size())),
            std::string(expression.constData(), static_cast<std::size_t>(expression.size())),
            std::string(unit.constData(), static_cast<std::size_t>(unit.size()))});
    }

    const auto previous = derivedFields_.definitions();
    const auto result = derivedFields_.setDefinitions(std::move(requested));
    if (!result.success()) {
        QStringList messages;
        for (const auto& issue : result.issues) {
            messages.push_back(
                tr("第 %1 行，第 %2 个字符：%3 [%4]")
                    .arg(static_cast<qulonglong>(issue.definitionIndex + 1))
                    .arg(static_cast<qulonglong>(issue.position + 1))
                    .arg(QString::fromStdString(issue.message),
                         QString::fromStdString(issue.code)));
        }
        emit derivedFieldsConfigured(false, messages);
        return false;
    }

    for (const auto& definition : previous) timeSeries_.clear(definition.name);
    for (const auto& definition : derivedFields_.definitions()) {
        timeSeries_.clear(definition.name);
    }
    const auto count = derivedFields_.definitions().size();
    emit derivedFieldsConfigured(
        true,
        {count == 0
             ? tr("派生变量已全部关闭")
             : tr("已应用 %1 个派生变量；新样本到达后会生成曲线")
                   .arg(static_cast<qulonglong>(count))});
    return true;
}

bool SerialSession::setAlertRules(const QVariantList& definitions) {
    if (recorder_.isRecording()) {
        emit alertRulesConfigured(
            false,
            {tr("Session 记录期间告警规则已冻结，请停止记录后再修改")});
        return false;
    }

    std::vector<lab::core::ThresholdAlertDefinition> requested;
    requested.reserve(static_cast<std::size_t>(definitions.size()));
    for (const auto& value : definitions) {
        const auto definition = value.toMap();
        const auto name = definition.value(QStringLiteral("name")).toString().trimmed();
        const auto field = definition.value(QStringLiteral("field")).toString().trimmed();
        const auto comparison =
            definition.value(QStringLiteral("comparison")).toString().trimmed();
        const auto message =
            definition.value(QStringLiteral("message")).toString().trimmed();
        const auto durationMs =
            definition.value(QStringLiteral("duration_ms"), 0).toLongLong();
        const auto durationNs = durationMs < 0
                                    ? static_cast<lab::core::Timestamp>(-1)
                                : durationMs > maximumAlertWindowMs
                                    ? static_cast<lab::core::Timestamp>(
                                          maximumAlertWindowMs) * 1'000'000 + 1
                                    : static_cast<lab::core::Timestamp>(
                                          durationMs) * 1'000'000;
        const auto toString = [](const QString& text) {
            const auto bytes = text.toUtf8();
            return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
        };
        requested.push_back({toString(name),
                             toString(field),
                             comparison == QStringLiteral("below")
                                 ? lab::core::ThresholdComparison::Below
                                 : comparison == QStringLiteral("above")
                                       ? lab::core::ThresholdComparison::Above
                                       : static_cast<lab::core::ThresholdComparison>(-1),
                             definition.value(QStringLiteral("threshold")).toDouble(),
                             definition.value(QStringLiteral("hysteresis")).toDouble(),
                             toString(message),
                             durationNs});
    }

    const auto result = alertRules_.setDefinitions(std::move(requested));
    if (!result.success()) {
        QStringList messages;
        for (const auto& issue : result.issues) {
            messages.push_back(
                tr("第 %1 行：%2 [%3]")
                    .arg(static_cast<qulonglong>(issue.definitionIndex + 1))
                    .arg(QString::fromStdString(issue.message),
                         QString::fromStdString(issue.code)));
        }
        emit alertRulesConfigured(false, messages);
        return false;
    }
    const auto count = alertRules_.definitions().size();
    emit alertRulesConfigured(
        true,
        {count == 0
             ? tr("阈值告警已全部关闭")
             : tr("已应用 %1 个阈值告警；告警采用边沿触发与回差复位")
                   .arg(static_cast<qulonglong>(count))});
    return true;
}

bool SerialSession::setHealthAlertRules(const QVariantList& definitions) {
    if (recorder_.isRecording()) {
        emit healthAlertRulesConfigured(
            false,
            {tr("Session 记录期间运行健康告警已冻结，请停止记录后再修改")});
        return false;
    }

    std::vector<lab::core::HealthAlertDefinition> requested;
    requested.reserve(static_cast<std::size_t>(definitions.size()));
    const auto toString = [](const QString& text) {
        const auto bytes = text.toUtf8();
        return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
    };
    for (const auto& value : definitions) {
        const auto definition = value.toMap();
        const auto kind = definition.value(QStringLiteral("kind")).toString();
        const auto windowMs =
            definition.value(QStringLiteral("window_ms")).toLongLong();
        const auto windowNs = windowMs < 0
                                  ? static_cast<lab::core::Timestamp>(-1)
                              : windowMs > maximumAlertWindowMs
                                  ? static_cast<lab::core::Timestamp>(
                                        maximumAlertWindowMs) * 1'000'000 + 1
                                  : static_cast<lab::core::Timestamp>(
                                        windowMs) * 1'000'000;
        requested.push_back({
            toString(definition.value(QStringLiteral("name")).toString().trimmed()),
            toString(definition.value(QStringLiteral("source_id")).toString().trimmed()),
            kind == QStringLiteral("error_rate")
                ? lab::core::HealthAlertKind::ErrorRate
            : kind == QStringLiteral("inactivity")
                ? lab::core::HealthAlertKind::Inactivity
                : static_cast<lab::core::HealthAlertKind>(-1),
            definition.value(QStringLiteral("error_count"), 1).toULongLong(),
            windowNs,
            toString(definition.value(QStringLiteral("message")).toString().trimmed())});
    }

    const auto result = healthAlertRules_.setDefinitions(std::move(requested));
    if (!result.success()) {
        QStringList messages;
        for (const auto& issue : result.issues) {
            messages.push_back(
                tr("第 %1 行：%2 [%3]")
                    .arg(static_cast<qulonglong>(issue.definitionIndex + 1))
                    .arg(QString::fromStdString(issue.message),
                         QString::fromStdString(issue.code)));
        }
        emit healthAlertRulesConfigured(false, messages);
        return false;
    }
    armHealthAlertTargets();
    const auto count = healthAlertRules_.definitions().size();
    emit healthAlertRulesConfigured(
        true,
        {count == 0
             ? tr("运行健康告警已全部关闭")
             : tr("已应用 %1 个运行健康告警；超时从来源打开或 Topic 订阅时开始计时")
                   .arg(static_cast<qulonglong>(count))});
    return true;
}

bool SerialSession::addManualMarker(const QString& message) {
    if (replaying_.load()) {
        emit sourceError(tr("回放 Session 是只读的，不能添加实时 Marker"));
        return false;
    }
    const auto cleaned = message.trimmed();
    const auto bytes = cleaned.toUtf8();
    if (bytes.isEmpty() || bytes.size() > 1024 || cleaned.contains(QLatin1Char('\n')) ||
        cleaned.contains(QLatin1Char('\r'))) {
        emit sourceError(tr("Marker 说明不能为空、换行或超过 1024 字节"));
        return false;
    }
    auto timestamp = latestLiveTimestamp_.load();
    if (timestamp == 0) timestamp = lab::core::nowTimestampNs();
    publishTimelineEvent({timestamp,
                          "lab_debugger",
                          "info",
                          "marker",
                          std::string(bytes.constData(),
                                      static_cast<std::size_t>(bytes.size())),
                          0},
                         recorder_.isRecording());
    return true;
}

void SerialSession::loadProtocolFile(const QString& path) {
    if (recorder_.isRecording()) {
        emit protocolLoadFailed(
            {tr("Session 记录期间解析配置已冻结，请停止记录后再修改")});
        return;
    }
    QStringList issues;
    auto loaded = readProtocolDefinition(path, issues);
    if (!loaded) {
        emit protocolLoadFailed(issues);
        return;
    }
    const auto protocolName = QString::fromStdString(loaded->definition.name);
    processing_.setProtocolDefinition(loaded->definition);
    activeProtocolName_ = loaded->definition.name;
    activeProtocolJson_ = std::move(loaded->json);
    resetParserDependentState();
    recorder_.updateProtocolSnapshot(
        activeProtocolName_, activeProtocolJson_, lab::core::nowTimestampNs());
    recorder_.enqueueEvent({lab::core::nowTimestampNs(),
                            "lab_debugger",
                            "info",
                            "protocol",
                            "Protocol loaded: " + activeProtocolName_,
                            0});
    emit protocolLoaded(protocolName, loaded->numericFields);
    emit sourceParserConfigured({},
                                QStringLiteral("protocol"),
                                protocolName,
                                loaded->numericFields);
}

void SerialSession::clearProtocol() {
    if (recorder_.isRecording()) {
        emit protocolLoadFailed(
            {tr("Session 记录期间解析配置已冻结，请停止记录后再修改")});
        return;
    }
    processing_.clearProtocolDefinition();
    activeProtocolName_.clear();
    activeProtocolJson_.clear();
    resetParserDependentState();
    recorder_.enqueueEvent({lab::core::nowTimestampNs(),
                            "lab_debugger",
                            "info",
                            "protocol",
                            "Protocol disabled",
                            0});
    emit protocolCleared();
    QStringList fields;
    for (const auto& field : activeCsvFields_) {
        fields.push_back(QString::fromStdString(field));
    }
    emit sourceParserConfigured({}, QStringLiteral("csv"), {}, fields);
}

bool SerialSession::startSession(const QString& directory) {
    if (replay_.isOpen()) {
        emit recordingChanged(false, tr("回放模式下不能开始新的实时 Session 记录"));
        return false;
    }
    const auto remoteAgentOpen = sourceManager_.isOpen(std::string(remoteAgentSourceKey));
    std::set<std::string> openSourceKeys;
    for (const auto& [sourceId, source] : serialSources_) {
        if (source->isOpen()) openSourceKeys.insert(sourceId);
    }
    for (const auto& [sourceId, source] : networkSources_) {
        if (source->isOpen()) openSourceKeys.insert(sourceId);
    }
    if (remoteAgentOpen) {
        openSourceKeys.insert(std::string(remoteAgentSourceKey));
    }
    if (openSourceKeys.empty()) {
        emit recordingChanged(false, tr("请先连接至少一个实时数据源"));
        return false;
    }
    lab::core::SessionStartOptions options;
    options.softwareVersion = "0.21.0";
    options.sessionName = QFileInfo(directory).fileName().toStdString();
    options.machineName = QSysInfo::machineHostName().toStdString();
    options.operatingSystem = QSysInfo::prettyProductName().toStdString();
    options.protocolName = activeProtocolName_;
    options.protocolJson = activeProtocolJson_;
    options.csvFields = activeCsvFields_;
    options.derivedFields = derivedFields_.definitions();
    options.alertRules = alertRules_.definitions();
    options.healthAlertRules = healthAlertRules_.definitions();
    for (const auto& [sourceId, source] : serialSources_) {
        if (!source->isOpen()) continue;
        const auto settings = source->settings();
        options.sources.push_back({
            sourceId,
            "serial",
            settings.portName,
            {{"port", settings.portName},
             {"baud_rate", std::to_string(settings.baudRate)},
             {"data_bits", std::to_string(settings.dataBits)},
             {"stop_bits", std::to_string(static_cast<int>(settings.stopBits))},
             {"parity", std::to_string(static_cast<int>(settings.parity))},
             {"flow_control", std::to_string(static_cast<int>(settings.flowControl))}},
            resolvedParserConfiguration(sourceId)});
    }
    for (const auto& [sourceId, source] : networkSources_) {
        if (!source->isOpen()) continue;
        const auto settings = source->settings();
        options.sources.push_back({
            sourceId,
            lab::adapters::network::toString(settings.mode),
            sourceId,
            {{"remote_host", settings.remoteHost},
             {"remote_port", std::to_string(settings.remotePort)},
             {"bind_address", settings.bindAddress},
             {"local_port", std::to_string(settings.localPort)}},
            resolvedParserConfiguration(sourceId)});
    }
    if (remoteAgentOpen) {
        options.sources.push_back({
            remoteAgent_.sourceId(),
            "ros_remote_agent",
            lastRemoteAgentSettings_.host + ':' +
                std::to_string(lastRemoteAgentSettings_.port),
             {{"host", lastRemoteAgentSettings_.host},
              {"port", std::to_string(lastRemoteAgentSettings_.port)},
              {"client_name", lastRemoteAgentSettings_.clientName},
              {"client_version", lastRemoteAgentSettings_.clientVersion},
              {"auto_reconnect",
               lastRemoteAgentSettings_.autoReconnect ? "true" : "false"},
              {"clock_sync_method", "ping_pong_min_rtt"},
              {"clock_sync_interval_ms", "2000"}},
            std::nullopt});
    }

    const auto sourceCount = options.sources.size();
    bool result = false;
    {
        std::scoped_lock routeLock(routingMutex_);
        processing_.flush();
        derivedFields_.resetValues();
        alertRules_.resetValues();
        healthAlertRules_.resetValues();
        armHealthAlertTargets();
        latestLiveTimestamp_.store(0);
        nextTimelineSequence_.store(0);
        clearTimelineEvents();
        {
            std::scoped_lock lock(recordingSourcesMutex_);
            recordingSourceKeys_ = openSourceKeys;
        }
        result = recorder_.start(
            std::filesystem::path(directory.toStdWString()), std::move(options));
        if (!result) {
            std::scoped_lock lock(recordingSourcesMutex_);
            recordingSourceKeys_.clear();
        }
    }
    const auto message = result
                             ? tr("Session 记录已开始：%1").arg(directory)
                             : tr("Session 创建失败：%1")
                                   .arg(QString::fromStdString(recorder_.error()));
    emit recordingChanged(result, message);
    if (result) {
        recorder_.enqueueEvent({lab::core::nowTimestampNs(),
                                "lab_debugger",
                                "info",
                                "session",
                                "Session recording started with " +
                                    std::to_string(sourceCount) + " live sources",
                                0});
    }
    return result;
}

void SerialSession::stopSession() {
    if (!recorder_.isRecording()) {
        return;
    }
    {
        std::scoped_lock routeLock(routingMutex_);
        processing_.flush();
        recorder_.enqueueEvent({lab::core::nowTimestampNs(),
                                "lab_debugger",
                                "info",
                                "session",
                                "Session recording stopped",
                                0});
        recorder_.stop();
        std::scoped_lock lock(recordingSourcesMutex_);
        recordingSourceKeys_.clear();
    }
    emit recordingChanged(false, tr("Session 记录已安全结束"));
}

bool SerialSession::openReplaySession(const QString& directory) {
    if (recorder_.isRecording()) {
        emit replayOpenFailed(tr("请先停止当前 Session 记录"));
        return false;
    }

    const QFileInfo selected(directory);
    const auto sessionDirectory = selected.isDir() ? selected.absoluteFilePath()
                                                   : selected.absolutePath();
    const auto rawPath = selected.isDir()
                             ? QDir(sessionDirectory).filePath(QStringLiteral("raw/stream.ldraw"))
                             : selected.absoluteFilePath();
    if (!QFileInfo::exists(rawPath)) {
        emit replayOpenFailed(tr("找不到 Session 原始数据：%1").arg(rawPath));
        return false;
    }

    const auto previousDerivedFields =
        derivedDefinitionsToVariant(derivedFields_.definitions());
    const auto previousAlertRules =
        alertDefinitionsToVariant(alertRules_.definitions());
    const auto previousHealthAlertRules =
        healthAlertDefinitionsToVariant(healthAlertRules_.definitions());

    sourceManager_.closeAll();
    replay_.close();
    replaying_.store(false);
    clearTimelineEvents();
    {
        std::scoped_lock routeLock(routingMutex_);
        processing_.flush();
        processing_.resetParsers();
        derivedFields_.resetValues();
        alertRules_.resetValues();
        healthAlertRules_.resetValues();
        timeSeries_.clear();
    }
    {
        std::scoped_lock lock(uiQueueMutex_);
        uiQueue_.clear();
    }
    {
        std::scoped_lock lock(liveFieldsMutex_);
        liveFieldNames_.clear();
    }
    const auto protocolPath = QDir(sessionDirectory).filePath(QStringLiteral("protocol/initial.json"));
    bool rawOnly = false;
    bool structuredRosbag = false;
    QJsonDocument metadataDocument;
    const auto metadataPath =
        QDir(sessionDirectory).filePath(QStringLiteral("metadata.json"));
    QFile metadataFile(metadataPath);
    if (QFileInfo::exists(metadataPath)) {
        if (!metadataFile.open(QIODevice::ReadOnly)) {
            emit replayOpenFailed(tr("无法读取 Session metadata"));
            return false;
        }
        const auto metadataSize = metadataFile.size();
        if (metadataSize <= 0 || metadataSize > maximumSessionMetadataBytes) {
            emit replayOpenFailed(tr("Session metadata 为空或超过 4 MiB 安全限制"));
            return false;
        }
        const auto metadataBytes = metadataFile.readAll();
        if (metadataFile.error() != QFileDevice::NoError ||
            metadataBytes.size() != metadataSize) {
            emit replayOpenFailed(tr("Session metadata 读取不完整"));
            return false;
        }
        QJsonParseError metadataError;
        metadataDocument = QJsonDocument::fromJson(metadataBytes, &metadataError);
        if (metadataError.error != QJsonParseError::NoError ||
            !metadataDocument.isObject()) {
            emit replayOpenFailed(tr("Session metadata 已损坏"));
            return false;
        }
        const auto replayModeValue =
            metadataDocument.object().value(QStringLiteral("replay_mode"));
        if (!replayModeValue.isUndefined() && !replayModeValue.isString()) {
            emit replayOpenFailed(tr("Session 回放路由类型无效"));
            return false;
        }
        const auto replayMode = replayModeValue.toString();
        if (!replayMode.isEmpty() && replayMode != QStringLiteral("raw-only") &&
            replayMode != QStringLiteral("rosbag2-structured")) {
            emit replayOpenFailed(tr("Session 回放路由模式不受支持"));
            return false;
        }
        rawOnly = replayMode == QStringLiteral("raw-only");
        structuredRosbag = replayMode == QStringLiteral("rosbag2-structured");
    }
    bool sourceParserFormatDeclared = false;
    if (metadataDocument.isObject()) {
        const auto format =
            metadataDocument.object().value(QStringLiteral("source_parser_format"));
        if (!format.isUndefined()) {
            if (!format.isDouble() || format.toDouble() != 1.0) {
                emit replayOpenFailed(tr("Session 逐来源解析格式版本不受支持"));
                return false;
            }
            sourceParserFormatDeclared = true;
        }
    }

    decltype(rosbagReplayTopics_) rosbagTopics;
    if (structuredRosbag) {
        QFile catalogFile(QDir(sessionDirectory).filePath(
            QStringLiteral("configuration/rosbag2.json")));
        if (!catalogFile.open(QIODevice::ReadOnly)) {
            emit replayOpenFailed(tr("结构化 rosbag2 Session 缺少 Topic 配置目录"));
            return false;
        }
        const auto catalog = QJsonDocument::fromJson(catalogFile.readAll());
        if (!catalog.isObject()) {
            emit replayOpenFailed(tr("结构化 rosbag2 Topic 配置已损坏"));
            return false;
        }
        for (const auto value :
             catalog.object().value(QStringLiteral("topics")).toArray()) {
            const auto item = value.toObject();
            if (item.value(QStringLiteral("field_mapping")).toString() !=
                QStringLiteral("built-in")) {
                continue;
            }
            const auto nameBytes =
                item.value(QStringLiteral("name")).toString().toUtf8();
            const auto typeBytes =
                item.value(QStringLiteral("type")).toString().toUtf8();
            std::string name(nameBytes.constData(),
                             static_cast<std::size_t>(nameBytes.size()));
            std::string type(typeBytes.constData(),
                             static_cast<std::size_t>(typeBytes.size()));
            if (name.empty() || type.empty()) continue;
            const auto source =
                lab::adapters::rosbag2::rosbag2SourceId(name, type);
            rosbagTopics.emplace(
                source, std::pair{std::move(name), std::move(type)});
        }
        if (rosbagTopics.empty()) {
            emit replayOpenFailed(tr("结构化 rosbag2 Session 没有可用的字段映射"));
            return false;
        }
    }
    {
        std::scoped_lock routeLock(routingMutex_);
        rosbagReplayTopics_ = std::move(rosbagTopics);
        replayFieldNames_.clear();
        replayMappingWarnings_.clear();
    }
    const auto safeCdrRouting = rawOnly || structuredRosbag;
    replayRawOnly_.store(safeCdrRouting);
    replayStructuredRosbag_.store(structuredRosbag);
    std::vector<RestoredParserConfiguration> restoredParserConfigurations;
    if (!safeCdrRouting && metadataDocument.isObject()) {
        const auto sourcesValue =
            metadataDocument.object().value(QStringLiteral("sources"));
        if (sourceParserFormatDeclared && !sourcesValue.isArray()) {
            emit replayOpenFailed(tr("Session 缺少逐来源数据源目录"));
            return false;
        }
        if (sourcesValue.isArray()) {
            const auto sources = sourcesValue.toArray();
            if (sources.size() > maximumParserSources) {
                emit replayOpenFailed(tr("Session 数据源超过 64 项安全限制"));
                return false;
            }
            std::set<std::string> sourceIds;
            for (qsizetype index = 0; index < sources.size(); ++index) {
                if (!sources[index].isObject()) {
                    emit replayOpenFailed(tr("Session 数据源目录包含无效条目"));
                    return false;
                }
                const auto metadataSource = sources[index].toObject();
                const auto metadataId = metadataSource.value(QStringLiteral("id"));
                const auto metadataType = metadataSource.value(QStringLiteral("type"));
                if (!metadataId.isString() || !metadataType.isString()) {
                    emit replayOpenFailed(tr("Session 数据源缺少有效标识"));
                    return false;
                }
                const auto type = metadataType.toString();
                const auto localByteSource =
                    type == QStringLiteral("serial") ||
                    type == QStringLiteral("tcp_client") ||
                    type == QStringLiteral("tcp_server") ||
                    type == QStringLiteral("udp");
                const auto sourceId = metadataId.toString();
                const auto sourceBytes = sourceId.toUtf8();
                if (sourceBytes.isEmpty() || sourceBytes.size() > 1024 ||
                    containsControlCharacter(sourceId) ||
                    !sourceIds.insert(utf8String(sourceId)).second) {
                    emit replayOpenFailed(tr("Session 数据源标识无效或重复"));
                    return false;
                }

                const auto configurationPath = QDir(sessionDirectory).filePath(
                    QStringLiteral("configuration/source_%1.json").arg(index));
                QFile configurationFile(configurationPath);
                if (!configurationFile.open(QIODevice::ReadOnly)) {
                    if (sourceParserFormatDeclared && localByteSource) {
                        emit replayOpenFailed(tr("Session 缺少逐来源解析配置"));
                        return false;
                    }
                    continue;
                }
                const auto configurationSize = configurationFile.size();
                if (configurationSize < 0 ||
                    configurationSize > maximumProtocolDefinitionBytes) {
                    emit replayOpenFailed(tr("Session 数据源配置超过 1 MiB 安全限制"));
                    return false;
                }
                const auto configurationBytes = configurationFile.readAll();
                if (configurationFile.error() != QFileDevice::NoError ||
                    configurationBytes.size() != configurationSize) {
                    emit replayOpenFailed(tr("Session 数据源配置读取不完整"));
                    return false;
                }
                QJsonParseError configurationError;
                const auto configurationDocument = QJsonDocument::fromJson(
                    configurationBytes, &configurationError);
                if (configurationError.error != QJsonParseError::NoError ||
                    !configurationDocument.isObject()) {
                    emit replayOpenFailed(tr("Session 数据源配置已损坏"));
                    return false;
                }
                const auto sourceObject = configurationDocument.object();
                if (!sourceObject.value(QStringLiteral("id")).isString() ||
                    sourceObject.value(QStringLiteral("id")).toString() != sourceId ||
                    !sourceObject.value(QStringLiteral("type")).isString() ||
                    sourceObject.value(QStringLiteral("type")).toString() != type) {
                    emit replayOpenFailed(tr("Session 数据源配置与目录标识不一致"));
                    return false;
                }
                const auto parserValue = sourceObject.value(QStringLiteral("parser"));
                if (parserValue.isUndefined()) {
                    if (sourceParserFormatDeclared && localByteSource) {
                        emit replayOpenFailed(tr("Session 数据源缺少解析器快照"));
                        return false;
                    }
                    continue;
                }
                if (!parserValue.isObject()) {
                    emit replayOpenFailed(tr("Session 逐来源解析配置类型无效"));
                    return false;
                }
                const auto parser = parserValue.toObject();
                const auto formatVersion = parser.value(QStringLiteral("format_version"));
                const auto mode = parser.value(QStringLiteral("mode"));
                const auto csvFields = parser.value(QStringLiteral("csv_fields"));
                const auto protocol = parser.value(QStringLiteral("protocol"));
                if (!formatVersion.isDouble() || formatVersion.toDouble() != 1.0 ||
                    !mode.isString() || !csvFields.isArray() || !protocol.isObject()) {
                    emit replayOpenFailed(
                        tr("Session 逐来源解析配置版本不受支持或字段缺失"));
                    return false;
                }
                QStringList fields;
                for (const auto& field : csvFields.toArray()) {
                    if (!field.isString()) {
                        emit replayOpenFailed(tr("Session CSV 字段配置类型无效"));
                        return false;
                    }
                    fields.push_back(field.toString());
                }
                QStringList fieldIssues;
                auto validatedFields = validatedCsvFields(fields, fieldIssues);
                if (!validatedFields) {
                    emit replayOpenFailed(
                        tr("Session CSV 字段配置未通过安全校验：%1")
                            .arg(fieldIssues.join(QLatin1Char(';'))));
                    return false;
                }

                RestoredParserConfiguration restored;
                restored.sourceId = sourceId;
                restored.csvFields = std::move(*validatedFields);
                const auto protocolObject = protocol.toObject();
                const auto protocolName = protocolObject.value(QStringLiteral("name"));
                const auto protocolSnapshot =
                    protocolObject.value(QStringLiteral("snapshot"));
                if (!protocolName.isString()) {
                    emit replayOpenFailed(tr("Session 逐来源协议名称类型无效"));
                    return false;
                }
                if (mode.toString() == QStringLiteral("csv")) {
                    if (!protocolSnapshot.isNull() || !protocolName.toString().isEmpty()) {
                        emit replayOpenFailed(tr("Session CSV 解析配置包含意外协议快照"));
                        return false;
                    }
                    restored.visibleFields = fields;
                } else if (mode.toString() == QStringLiteral("protocol")) {
                    const auto expectedSnapshot =
                        QStringLiteral("protocol/source_%1_initial.json").arg(index);
                    if (!protocolSnapshot.isString() ||
                        protocolSnapshot.toString() != expectedSnapshot ||
                        protocolName.toString().isEmpty()) {
                        emit replayOpenFailed(tr("Session 逐来源协议快照引用无效"));
                        return false;
                    }
                    QStringList protocolIssues;
                    auto loaded = readProtocolDefinition(
                        QDir(sessionDirectory).filePath(expectedSnapshot), protocolIssues);
                    if (!loaded ||
                        QString::fromStdString(loaded->definition.name) !=
                            protocolName.toString()) {
                        emit replayOpenFailed(
                            tr("Session 逐来源协议快照无效：%1")
                                .arg(protocolIssues.join(QLatin1Char(';'))));
                        return false;
                    }
                    restored.protocolName = loaded->definition.name;
                    restored.protocolJson = std::move(loaded->json);
                    restored.protocolDefinition = std::move(loaded->definition);
                    restored.visibleFields = std::move(loaded->numericFields);
                } else {
                    emit replayOpenFailed(tr("Session 逐来源解析模式不受支持"));
                    return false;
                }
                restoredParserConfigurations.push_back(std::move(restored));
            }
        }
    }
    clearSourceParserConfigurations();
    clearProtocol();
    if (!safeCdrRouting && QFileInfo::exists(protocolPath)) {
        loadProtocolFile(protocolPath);
    }
    if (!safeCdrRouting && activeProtocolName_.empty()) {
        QFile fieldsFile(QDir(sessionDirectory).filePath(
            QStringLiteral("configuration/csv_fields.txt")));
        if (fieldsFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QStringList restoredFields;
            while (!fieldsFile.atEnd()) {
                const auto field = QString::fromUtf8(fieldsFile.readLine()).trimmed();
                if (!field.isEmpty()) {
                    restoredFields.push_back(field);
                }
            }
            if (!restoredFields.isEmpty()) {
                setCsvFields(restoredFields);
                emit csvFieldsRestored(restoredFields);
            }
        }
    }
    for (auto& restored : restoredParserConfigurations) {
        lab::core::ProcessingPipeline::ParserConfiguration configuration;
        configuration.fieldNames = restored.csvFields;
        configuration.protocolDefinition = restored.protocolDefinition;
        const auto sourceId = utf8String(restored.sourceId);
        if (!processing_.setSourceConfiguration(sourceId, std::move(configuration))) {
            emit replayOpenFailed(tr("Session 逐来源解析配置无法应用"));
            return false;
        }
        sourceParserConfigurations_.insert_or_assign(
            sourceId,
            ActiveParserConfiguration{std::move(restored.csvFields),
                                      std::move(restored.protocolDefinition),
                                      std::move(restored.protocolName),
                                      std::move(restored.protocolJson)});
        const auto& active = sourceParserConfigurations_.at(sourceId);
        emit sourceParserConfigured(
            restored.sourceId,
            active.protocolDefinition ? QStringLiteral("protocol")
                                      : QStringLiteral("csv"),
            QString::fromStdString(active.protocolName),
            restored.visibleFields);
    }
    publishParserSources();
    QVariantList restoredDerivedFields;
    const auto derivedPath = QDir(sessionDirectory).filePath(
        QStringLiteral("configuration/derived_fields.json"));
    if (QFileInfo::exists(derivedPath)) {
        QFile derivedFile(derivedPath);
        if (!derivedFile.open(QIODevice::ReadOnly)) {
            emit replayOpenFailed(tr("无法读取 Session 派生变量配置"));
            return false;
        }
        const auto declaredSize = derivedFile.size();
        if (declaredSize < 0 || declaredSize > maximumDerivedConfigurationBytes) {
            emit replayOpenFailed(tr("Session 派生变量配置超过 1 MiB 安全限制"));
            return false;
        }
        const auto contents = derivedFile.readAll();
        if (derivedFile.error() != QFileDevice::NoError ||
            contents.size() != declaredSize) {
            emit replayOpenFailed(tr("Session 派生变量配置读取不完整"));
            return false;
        }
        const auto document = QJsonDocument::fromJson(contents);
        if (!document.isObject()) {
            emit replayOpenFailed(tr("Session 派生变量配置已损坏"));
            return false;
        }
        const auto object = document.object();
        const auto formatVersion = object.value(QStringLiteral("format_version"));
        const auto fieldsValue = object.value(QStringLiteral("fields"));
        if (!formatVersion.isDouble() || formatVersion.toDouble() != 1.0 ||
            !fieldsValue.isArray()) {
            emit replayOpenFailed(tr("Session 派生变量配置格式版本不受支持或字段缺失"));
            return false;
        }
        const auto fields = fieldsValue.toArray();
        if (fields.size() > maximumDerivedDefinitions) {
            emit replayOpenFailed(tr("Session 派生变量配置超过 128 项安全限制"));
            return false;
        }
        for (const auto& value : fields) {
            if (!value.isObject()) {
                emit replayOpenFailed(tr("Session 派生变量配置包含无效条目"));
                return false;
            }
            const auto field = value.toObject();
            const auto name = field.value(QStringLiteral("name"));
            const auto expression = field.value(QStringLiteral("expression"));
            const auto unit = field.value(QStringLiteral("unit"));
            if (!name.isString() || !expression.isString() || !unit.isString()) {
                emit replayOpenFailed(tr("Session 派生变量配置条目类型无效"));
                return false;
            }
            QVariantMap restored;
            restored.insert(QStringLiteral("name"), name.toString());
            restored.insert(QStringLiteral("expression"), expression.toString());
            restored.insert(QStringLiteral("unit"), unit.toString());
            restoredDerivedFields.push_back(restored);
        }
    }
    QVariantList restoredAlertRules;
    const auto alertPath = QDir(sessionDirectory).filePath(
        QStringLiteral("configuration/alert_rules.json"));
    if (QFileInfo::exists(alertPath)) {
        QFile alertFile(alertPath);
        if (!alertFile.open(QIODevice::ReadOnly)) {
            emit replayOpenFailed(tr("无法读取 Session 告警规则配置"));
            return false;
        }
        const auto declaredSize = alertFile.size();
        if (declaredSize < 0 || declaredSize > maximumAlertConfigurationBytes) {
            emit replayOpenFailed(tr("Session 告警规则配置超过 1 MiB 安全限制"));
            return false;
        }
        const auto contents = alertFile.readAll();
        if (alertFile.error() != QFileDevice::NoError ||
            contents.size() != declaredSize) {
            emit replayOpenFailed(tr("Session 告警规则配置读取不完整"));
            return false;
        }
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(contents, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            emit replayOpenFailed(tr("Session 告警规则配置已损坏"));
            return false;
        }
        const auto object = document.object();
        const auto formatVersion = object.value(QStringLiteral("format_version"));
        const auto rulesValue = object.value(QStringLiteral("rules"));
        if (!formatVersion.isDouble() ||
            (formatVersion.toDouble() != 1.0 &&
             formatVersion.toDouble() != 2.0) ||
            !rulesValue.isArray()) {
            emit replayOpenFailed(tr("Session 告警规则格式版本不受支持或规则缺失"));
            return false;
        }
        const auto rules = rulesValue.toArray();
        if (rules.size() > maximumAlertDefinitions) {
            emit replayOpenFailed(tr("Session 告警规则超过 128 项安全限制"));
            return false;
        }
        for (const auto& value : rules) {
            if (!value.isObject()) {
                emit replayOpenFailed(tr("Session 告警规则包含无效条目"));
                return false;
            }
            const auto rule = value.toObject();
            const auto name = rule.value(QStringLiteral("name"));
            const auto field = rule.value(QStringLiteral("field"));
            const auto comparison = rule.value(QStringLiteral("comparison"));
            const auto threshold = rule.value(QStringLiteral("threshold"));
            const auto hysteresis = rule.value(QStringLiteral("hysteresis"));
            const auto duration = rule.value(QStringLiteral("duration_ms"));
            const auto message = rule.value(QStringLiteral("message"));
            if (!name.isString() || !field.isString() || !comparison.isString() ||
                !threshold.isDouble() || !hysteresis.isDouble() ||
                !message.isString() || !std::isfinite(threshold.toDouble()) ||
                !std::isfinite(hysteresis.toDouble()) ||
                (formatVersion.toDouble() == 2.0 &&
                 (!duration.isDouble() ||
                  !std::isfinite(duration.toDouble()) ||
                  std::floor(duration.toDouble()) != duration.toDouble() ||
                  duration.toDouble() < 0.0 ||
                  duration.toDouble() >
                      static_cast<double>(maximumAlertWindowMs)))) {
                emit replayOpenFailed(tr("Session 告警规则条目类型无效"));
                return false;
            }
            QVariantMap restored;
            restored.insert(QStringLiteral("name"), name.toString());
            restored.insert(QStringLiteral("field"), field.toString());
            restored.insert(QStringLiteral("comparison"), comparison.toString());
            restored.insert(QStringLiteral("threshold"), threshold.toDouble());
            restored.insert(QStringLiteral("hysteresis"), hysteresis.toDouble());
            restored.insert(QStringLiteral("duration_ms"),
                            formatVersion.toDouble() == 2.0
                                ? duration.toDouble()
                                : 0.0);
            restored.insert(QStringLiteral("message"), message.toString());
            restoredAlertRules.push_back(restored);
        }
    }

    QVariantList restoredHealthAlertRules;
    const auto healthAlertPath = QDir(sessionDirectory).filePath(
        QStringLiteral("configuration/health_alert_rules.json"));
    if (QFileInfo::exists(healthAlertPath)) {
        QFile healthAlertFile(healthAlertPath);
        if (!healthAlertFile.open(QIODevice::ReadOnly)) {
            emit replayOpenFailed(tr("无法读取 Session 运行健康告警配置"));
            return false;
        }
        const auto declaredSize = healthAlertFile.size();
        if (declaredSize < 0 ||
            declaredSize > maximumHealthAlertConfigurationBytes) {
            emit replayOpenFailed(
                tr("Session 运行健康告警配置超过 1 MiB 安全限制"));
            return false;
        }
        const auto contents = healthAlertFile.readAll();
        if (healthAlertFile.error() != QFileDevice::NoError ||
            contents.size() != declaredSize) {
            emit replayOpenFailed(tr("Session 运行健康告警配置读取不完整"));
            return false;
        }
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(contents, &parseError);
        if (parseError.error != QJsonParseError::NoError ||
            !document.isObject()) {
            emit replayOpenFailed(tr("Session 运行健康告警配置已损坏"));
            return false;
        }
        const auto object = document.object();
        const auto formatVersion = object.value(QStringLiteral("format_version"));
        const auto rulesValue = object.value(QStringLiteral("rules"));
        if (!formatVersion.isDouble() || formatVersion.toDouble() != 1.0 ||
            !rulesValue.isArray()) {
            emit replayOpenFailed(
                tr("Session 运行健康告警格式版本不受支持或规则缺失"));
            return false;
        }
        const auto rules = rulesValue.toArray();
        if (rules.size() > maximumHealthAlertDefinitions) {
            emit replayOpenFailed(
                tr("Session 运行健康告警超过 128 项安全限制"));
            return false;
        }
        for (const auto& value : rules) {
            if (!value.isObject()) {
                emit replayOpenFailed(
                    tr("Session 运行健康告警包含无效条目"));
                return false;
            }
            const auto rule = value.toObject();
            const auto name = rule.value(QStringLiteral("name"));
            const auto sourceId = rule.value(QStringLiteral("source_id"));
            const auto kind = rule.value(QStringLiteral("kind"));
            const auto errorCount = rule.value(QStringLiteral("error_count"));
            const auto window = rule.value(QStringLiteral("window_ms"));
            const auto message = rule.value(QStringLiteral("message"));
            if (!name.isString() || !sourceId.isString() || !kind.isString() ||
                (kind.toString() != QStringLiteral("error_rate") &&
                 kind.toString() != QStringLiteral("inactivity")) ||
                !errorCount.isDouble() ||
                !std::isfinite(errorCount.toDouble()) ||
                std::floor(errorCount.toDouble()) != errorCount.toDouble() ||
                errorCount.toDouble() < 1.0 || errorCount.toDouble() > 10'000.0 ||
                !window.isDouble() || !std::isfinite(window.toDouble()) ||
                std::floor(window.toDouble()) != window.toDouble() ||
                window.toDouble() < 1.0 ||
                window.toDouble() >
                    static_cast<double>(maximumAlertWindowMs) ||
                !message.isString()) {
                emit replayOpenFailed(
                    tr("Session 运行健康告警条目类型或范围无效"));
                return false;
            }
            QVariantMap restored;
            restored.insert(QStringLiteral("name"), name.toString());
            restored.insert(QStringLiteral("source_id"), sourceId.toString());
            restored.insert(QStringLiteral("kind"), kind.toString());
            restored.insert(QStringLiteral("error_count"),
                            errorCount.toDouble());
            restored.insert(QStringLiteral("window_ms"), window.toDouble());
            restored.insert(QStringLiteral("message"), message.toString());
            restoredHealthAlertRules.push_back(restored);
        }
    }

    std::deque<lab::core::SessionEvent> restoredEvents;
    const auto eventPath = QDir(sessionDirectory).filePath(QStringLiteral("events.jsonl"));
    if (QFileInfo::exists(eventPath)) {
        QFile eventFile(eventPath);
        if (!eventFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            emit replayOpenFailed(tr("无法读取 Session Marker/告警记录"));
            return false;
        }
        if (eventFile.size() < 0 || eventFile.size() > maximumEventLogBytes) {
            emit replayOpenFailed(tr("Session 事件记录超过 64 MiB 安全限制"));
            return false;
        }
        while (!eventFile.atEnd()) {
            const auto line = eventFile.readLine(maximumEventLineBytes + 2);
            if (line.size() > maximumEventLineBytes ||
                (!line.endsWith('\n') && !eventFile.atEnd())) {
                emit replayOpenFailed(tr("Session 事件记录包含超长行"));
                return false;
            }
            if (line.trimmed().isEmpty()) continue;
            QJsonParseError parseError;
            const auto document = QJsonDocument::fromJson(line, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                emit replayOpenFailed(tr("Session 事件记录包含损坏的 JSON 行"));
                return false;
            }
            const auto object = document.object();
            const auto source = object.value(QStringLiteral("source_id"));
            const auto severity = object.value(QStringLiteral("severity"));
            const auto category = object.value(QStringLiteral("category"));
            const auto message = object.value(QStringLiteral("message"));
            if (!source.isString() || !severity.isString() || !category.isString() ||
                !message.isString()) {
                emit replayOpenFailed(tr("Session 事件记录字段类型无效"));
                return false;
            }
            if (category.toString() != QStringLiteral("marker") &&
                category.toString() != QStringLiteral("alert")) {
                continue;
            }
            const std::string_view bytes(line.constData(),
                                         static_cast<std::size_t>(line.size()));
            const auto timestamp = parseTimestampField(bytes);
            if (!timestamp || *timestamp <= 0 ||
                source.toString().toUtf8().size() > 1024 ||
                severity.toString().toUtf8().size() > 64 ||
                message.toString().toUtf8().size() > 4096) {
                emit replayOpenFailed(tr("Session Marker/告警记录未通过安全校验"));
                return false;
            }
            if (restoredEvents.size() ==
                static_cast<std::size_t>(maximumTimelineEvents)) {
                emit replayOpenFailed(tr("Session Marker/告警超过 10000 项安全限制"));
                return false;
            }
            restoredEvents.push_back({*timestamp,
                                      source.toString().toStdString(),
                                      severity.toString().toStdString(),
                                      category.toString().toStdString(),
                                      message.toString().toStdString(),
                                      static_cast<std::uint64_t>(restoredEvents.size())});
        }
        if (eventFile.error() != QFileDevice::NoError) {
            emit replayOpenFailed(tr("Session 事件记录读取失败"));
            return false;
        }
    }
    if (!setHealthAlertRules(restoredHealthAlertRules)) {
        emit replayOpenFailed(
            tr("Session 运行健康告警无法通过安全校验"));
        return false;
    }
    if (!setAlertRules(restoredAlertRules)) {
        static_cast<void>(setHealthAlertRules(previousHealthAlertRules));
        emit replayOpenFailed(tr("Session 告警规则无法通过安全校验"));
        return false;
    }
    if (!setDerivedFields(restoredDerivedFields)) {
        static_cast<void>(setAlertRules(previousAlertRules));
        static_cast<void>(setHealthAlertRules(previousHealthAlertRules));
        emit replayOpenFailed(tr("Session 派生变量配置无法通过安全校验"));
        return false;
    }
    replay_.setPath(std::filesystem::path(rawPath.toStdWString()));
    if (!replay_.open()) {
        replayRawOnly_.store(false);
        replayStructuredRosbag_.store(false);
        {
            std::scoped_lock routeLock(routingMutex_);
            rosbagReplayTopics_.clear();
        }
        static_cast<void>(setAlertRules(previousAlertRules));
        static_cast<void>(setHealthAlertRules(previousHealthAlertRules));
        static_cast<void>(setDerivedFields(previousDerivedFields));
        emit replayOpenFailed(tr("无法打开回放文件；详细原因已写入状态栏和日志"));
        return false;
    }
    QVariantList restoredEventValues;
    {
        std::scoped_lock lock(timelineEventsMutex_);
        timelineEvents_ = std::move(restoredEvents);
        restoredEventValues.reserve(static_cast<qsizetype>(timelineEvents_.size()));
        for (const auto& event : timelineEvents_) {
            restoredEventValues.push_back(eventToVariant(event));
        }
    }
    nextTimelineSequence_.store(
        static_cast<std::uint64_t>(restoredEventValues.size()));
    replaying_.store(true);
    emit derivedFieldsRestored(restoredDerivedFields);
    emit alertRulesRestored(restoredAlertRules);
    emit healthAlertRulesRestored(restoredHealthAlertRules);
    emit timelineEventsChanged(restoredEventValues);
    const auto replayStatus = replay_.status();
    emit replayOpened(sessionDirectory,
                      replayStatus.recoveredTruncatedTail,
                      rawOnly,
                      structuredRosbag);
    return true;
}

void SerialSession::inspectRosbag2(const QString& source, const QString& destination) {
    if (recorder_.isRecording()) {
        emit rosbagInspectionFinished(false,
                                      source,
                                      destination,
                                      tr("请先停止当前 Session 记录"),
                                      {},
                                      0,
                                      0);
        return;
    }
    if (rosbagImporting_.exchange(true)) {
        emit rosbagInspectionFinished(false,
                                      source,
                                      destination,
                                      tr("已有 rosbag2 任务正在运行"),
                                      {},
                                      0,
                                      0);
        return;
    }
    if (rosbagImportWorker_.joinable()) {
        rosbagImportWorker_.join();
    }

    emit rosbagInspectionStarted(source, destination);
    rosbagImportWorker_ = std::jthread(
        [this, source, destination](std::stop_token stopToken) {
            const auto result = lab::adapters::rosbag2::inspectRosbag2(
                std::filesystem::path(source.toStdWString()),
                [stopToken] { return stopToken.stop_requested(); });
            QMetaObject::invokeMethod(
                this,
                [this, source, destination, result] {
                    rosbagImporting_.store(false);
                    QVariantList topics;
                    topics.reserve(static_cast<qsizetype>(result.topics.size()));
                    for (const auto& topic : result.topics) {
                        QVariantMap item;
                        item.insert(QStringLiteral("name"),
                                    QString::fromUtf8(topic.name));
                        item.insert(QStringLiteral("type"),
                                    QString::fromUtf8(topic.type));
                        item.insert(QStringLiteral("serialization_format"),
                                    QString::fromUtf8(topic.serializationFormat));
                        item.insert(QStringLiteral("message_count"),
                                    static_cast<qulonglong>(topic.messageCount));
                        item.insert(QStringLiteral("structured_fields"),
                                    topic.structuredFields);
                        topics.push_back(item);
                    }
                    QString successMessage;
                    if (result.success) {
                        if (!result.metadata.present) {
                            successMessage =
                                tr("Topic 目录读取完成；未发现 metadata.yaml，统计以 SQLite 为准");
                        } else if (result.metadata.warnings.empty()) {
                            successMessage =
                                tr("Topic 目录读取完成；metadata.yaml 已通过数据库交叉校验");
                        } else {
                            successMessage =
                                tr("Topic 目录读取完成；metadata.yaml 有 %1 项校验警告，统计仍以 SQLite 为准")
                                    .arg(result.metadata.warnings.size());
                        }
                    }
                    const auto message = result.cancelled
                                             ? tr("rosbag2 目录读取已取消")
                                             : result.success
                                                   ? successMessage
                                                   : tr("rosbag2 目录读取失败：%1")
                                                         .arg(QString::fromStdString(
                                                             result.error));
                    emit rosbagInspectionFinished(result.success,
                                                  source,
                                                  destination,
                                                  message,
                                                  topics,
                                                  result.databaseCount,
                                                  result.messageCount);
                },
                Qt::QueuedConnection);
        });
}

void SerialSession::importRosbag2(const QString& source,
                                  const QString& destination,
                                  const QVariantList& selectedTopics) {
    if (recorder_.isRecording()) {
        emit rosbagImportFinished(false,
                                  {},
                                  tr("请先停止当前 Session 记录"),
                                  0,
                                  0);
        return;
    }
    if (selectedTopics.isEmpty()) {
        emit rosbagImportFinished(false,
                                  {},
                                  tr("请至少选择一个可导入的 Topic"),
                                  0,
                                  0);
        return;
    }
    if (rosbagImporting_.exchange(true)) {
        emit rosbagImportFinished(false, {}, tr("已有 rosbag2 正在导入"), 0, 0);
        return;
    }
    if (rosbagImportWorker_.joinable()) {
        rosbagImportWorker_.join();
    }

    emit rosbagImportStarted(source, destination);
    rosbagImportWorker_ = std::jthread(
        [this, source, destination, selectedTopics](std::stop_token stopToken) {
            lab::adapters::rosbag2::Rosbag2ImportOptions options;
            options.source = std::filesystem::path(source.toStdWString());
            options.destination = std::filesystem::path(destination.toStdWString());
            options.sessionName = QFileInfo(destination).fileName().toStdString();
            options.softwareVersion = "0.21.0";
            options.includedTopics.reserve(
                static_cast<std::size_t>(selectedTopics.size()));
            for (const auto& selectedValue : selectedTopics) {
                const auto selected = selectedValue.toMap();
                const auto nameBytes =
                    selected.value(QStringLiteral("name")).toString().toUtf8();
                const auto typeBytes =
                    selected.value(QStringLiteral("type")).toString().toUtf8();
                options.includedTopics.push_back(
                    {std::string(nameBytes.constData(),
                                 static_cast<std::size_t>(nameBytes.size())),
                     std::string(typeBytes.constData(),
                                 static_cast<std::size_t>(typeBytes.size()))});
            }

            std::uint64_t lastReported = std::numeric_limits<std::uint64_t>::max();
            auto result = lab::adapters::rosbag2::importRosbag2(
                options,
                [this, stopToken, &lastReported](std::uint64_t imported,
                                                std::uint64_t total) {
                    if (stopToken.stop_requested()) {
                        return false;
                    }
                    if (imported == total || imported == 0 ||
                        imported / 1000 != lastReported / 1000) {
                        lastReported = imported;
                        QMetaObject::invokeMethod(
                            this,
                            [this, imported, total] {
                                emit rosbagImportProgress(imported, total);
                            },
                            Qt::QueuedConnection);
                    }
                    return true;
                });

            QMetaObject::invokeMethod(
                this,
                [this, result = std::move(result)] {
                    rosbagImporting_.store(false);
                    const auto directory = QString::fromStdWString(
                        result.sessionDirectory.wstring());
                    if (!result.success) {
                        const auto message = result.cancelled
                                                 ? tr("rosbag2 导入已取消")
                                                 : tr("rosbag2 导入失败：%1")
                                                       .arg(QString::fromStdString(result.error));
                        emit rosbagImportFinished(false, {}, message, 0, 0);
                        return;
                    }
                    auto message =
                        tr("已导入 %1 条消息、%2 个 Topic，生成 %3 个曲线采样点")
                            .arg(result.messageCount)
                            .arg(static_cast<quint64>(result.topics.size()))
                            .arg(result.sampleCount);
                    if (result.metadata.present) {
                        message += result.metadata.warnings.empty()
                                       ? tr("；metadata.yaml 已完整保留并通过校验")
                                       : tr("；metadata.yaml 已完整保留（%1 项校验警告，未覆盖 SQLite 统计）")
                                             .arg(result.metadata.warnings.size());
                    }
                    emit rosbagImportFinished(true,
                                              directory,
                                              message,
                                              result.messageCount,
                                              static_cast<quint64>(result.topics.size()));
                    openReplaySession(directory);
                },
                Qt::QueuedConnection);
        });
}

void SerialSession::cancelRosbag2Import() {
    if (rosbagImportWorker_.joinable() && rosbagImporting_.load()) {
        rosbagImportWorker_.request_stop();
    }
}

void SerialSession::closeReplay() {
    replay_.close();
    replaying_.store(false);
    replayRawOnly_.store(false);
    replayStructuredRosbag_.store(false);
    std::scoped_lock routeLock(routingMutex_);
    rosbagReplayTopics_.clear();
    replayFieldNames_.clear();
    replayMappingWarnings_.clear();
    derivedFields_.resetValues();
    alertRules_.resetValues();
    healthAlertRules_.resetValues();
    armHealthAlertTargets();
    clearTimelineEvents();
}

void SerialSession::pauseReplay() {
    replay_.pause();
}

void SerialSession::resumeReplay() {
    replay_.resume();
}

void SerialSession::setReplaySpeed(double speed) {
    replay_.setSpeed(speed);
}

void SerialSession::seekReplay(double fraction) {
    const auto wasPaused = replay_.status().paused;
    replay_.pause();
    {
        std::scoped_lock routeLock(routingMutex_);
        processing_.flush();
        processing_.resetParsers();
        derivedFields_.resetValues();
        alertRules_.resetValues();
        healthAlertRules_.resetValues();
        replay_.seekFraction(fraction);
        timeSeries_.clear();
        {
            std::scoped_lock lock(protocolQueueMutex_);
            protocolQueue_.clear();
        }
    }
    if (!wasPaused) {
        replay_.resume();
    }
}

void SerialSession::drainUiQueue() {
    processHealthAlerts();
    std::deque<lab::core::DataChunk> batch;
    {
        std::scoped_lock lock(uiQueueMutex_);
        constexpr std::size_t maximumBatch = 1000;
        const auto count = std::min(maximumBatch, uiQueue_.size());
        for (std::size_t index = 0; index < count; ++index) {
            batch.push_back(std::move(uiQueue_.front()));
            uiQueue_.pop_front();
        }
    }
    for (const auto& chunk : batch) {
        emit chunkReady(
            QByteArray(
                reinterpret_cast<const char*>(chunk.payload.data()),
                static_cast<qsizetype>(chunk.payload.size())),
            chunk.direction == lab::core::Direction::Tx,
            chunk.receiveTimestamp,
            QString::fromStdString(chunk.sourceId));
    }

    std::deque<lab::core::FrameEvent> frameEvents;
    {
        std::scoped_lock lock(protocolQueueMutex_);
        constexpr std::size_t maximumEvents = 200;
        const auto count = std::min(maximumEvents, protocolQueue_.size());
        for (std::size_t index = 0; index < count; ++index) {
            frameEvents.push_back(std::move(protocolQueue_.front()));
            protocolQueue_.pop_front();
        }
    }

    if (!frameEvents.empty()) {
        QVariantList events;
        events.reserve(static_cast<qsizetype>(frameEvents.size()));
        for (const auto& event : frameEvents) {
            QVariantMap item;
            switch (event.kind) {
            case lab::core::FrameEventKind::FrameDecoded:
                item.insert(QStringLiteral("kind"), QStringLiteral("frame"));
                break;
            case lab::core::FrameEventKind::GarbageDiscarded:
                item.insert(QStringLiteral("kind"), QStringLiteral("garbage"));
                break;
            case lab::core::FrameEventKind::ChecksumError:
                item.insert(QStringLiteral("kind"), QStringLiteral("checksum_error"));
                break;
            case lab::core::FrameEventKind::LengthError:
                item.insert(QStringLiteral("kind"), QStringLiteral("length_error"));
                break;
            case lab::core::FrameEventKind::DecodeError:
                item.insert(QStringLiteral("kind"), QStringLiteral("decode_error"));
                break;
            }
            item.insert(QStringLiteral("timestampNs"),
                        static_cast<qlonglong>(event.sourceTimestamp));
            item.insert(QStringLiteral("raw"),
                        QByteArray(reinterpret_cast<const char*>(event.rawBytes.data()),
                                   static_cast<qsizetype>(event.rawBytes.size())));
            item.insert(QStringLiteral("message"), QString::fromStdString(event.message));

            QVariantList fields;
            fields.reserve(static_cast<qsizetype>(event.fields.size()));
            for (const auto& decoded : event.fields) {
                QVariantMap field;
                field.insert(QStringLiteral("name"), QString::fromStdString(decoded.name));
                field.insert(QStringLiteral("value"),
                             QString::fromStdString(lab::core::decodedValueToString(decoded)));
                field.insert(QStringLiteral("unit"), QString::fromStdString(decoded.unit));
                field.insert(QStringLiteral("type"),
                             QString::fromStdString(lab::core::toString(decoded.type)));
                fields.push_back(field);
            }
            item.insert(QStringLiteral("fields"), fields);
            events.push_back(item);
        }
        emit protocolEventsReady(events);
    }

    if (const auto protocolStats = processing_.protocolStatistics()) {
        emit protocolStatisticsChanged(protocolStats->decodedFrames,
                                       protocolStats->discardedBytes,
                                       protocolStats->checksumErrors,
                                       protocolStats->lengthErrors,
                                       protocolStats->decodeErrors);
    }
    const auto replayStatus = replay_.status();
    emit replayStatusChanged(replayStatus.open,
                             replayStatus.paused,
                             replayStatus.atEnd,
                             replayStatus.speed,
                             replayStatus.position,
                             replayStatus.recordCount,
                             replayStatus.firstTimestamp,
                             replayStatus.lastTimestamp,
                             replayStatus.currentTimestamp);

    const auto stats = replayStatus.open ? replay_.statistics()
                                         : sourceManager_.aggregateStatistics();
    emit statisticsChanged(
        stats.receivedBytes,
        stats.transmittedBytes,
        static_cast<qsizetype>(processing_.pendingChunks()));
}

}  // namespace lab::app

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
#include <filesystem>
#include <limits>
#include <string_view>

namespace lab::app {
namespace {

constexpr std::string_view serialSourceKey = "serial";
constexpr std::string_view networkSourceKey = "network";
constexpr std::string_view remoteAgentSourceKey = "remote_agent";

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

}  // namespace

SerialSession::SerialSession(QObject* parent) : QObject(parent) {
    sourceManager_.setCallbacks({
        [this](const std::string& key, const lab::core::DataChunk& chunk) {
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
            const auto label = key == serialSourceKey
                                   ? "Serial"
                                   : key == networkSourceKey ? "Network" : "Remote Agent";
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
                [this, key, state] {
                    if (key == serialSourceKey) {
                        emit sourceStateChanged(static_cast<int>(state));
                    } else if (key == networkSourceKey) {
                        emit networkStateChanged(static_cast<int>(state));
                    } else {
                        emit remoteAgentStateChanged(static_cast<int>(state));
                    }
                },
                Qt::QueuedConnection);
        },
        [this](const std::string& key,
               const std::string& sourceId,
               const std::string& message) {
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
                [this, key, message] {
                    const auto detail = QString::fromStdString(message);
                    if (key == networkSourceKey) {
                        emit sourceError(tr("网络：%1").arg(detail));
                    } else if (key == remoteAgentSourceKey) {
                        emit sourceError(tr("Remote Agent：%1").arg(detail));
                    } else {
                        emit sourceError(detail);
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
                timeSeries_.append(sample);
                if (isDeclaredRecordingSource(key)) {
                    recorder_.enqueueSample(sample);
                }
            }
            discoverLiveField(sample.field);
        }});
    static_cast<void>(sourceManager_.add(std::string(serialSourceKey), source_));
    static_cast<void>(sourceManager_.add(std::string(networkSourceKey), network_));
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
        {},
        {},
        [this](const lab::core::ClockSyncEstimate& estimate) {
            if (recordingRemoteAgent_.load()) {
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
                            for (const auto& field : mapping.fields) {
                                const auto name = topic->second.first + "." + field.path;
                                timeSeries_.append({timestamp,
                                                    chunk.sourceId,
                                                    name,
                                                    field.value,
                                                    field.unit,
                                                    chunk.sequence});
                                changed = replayFieldNames_.insert(name).second || changed;
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
        }});

    processing_.setQualifyFieldNames(true);
    processing_.setSampleHandler([this](const lab::core::DataSample& sample) {
        recorder_.enqueueSample(sample);
        discoverLiveField(sample.field);
    });

    processing_.setFrameHandler([this](const lab::core::FrameEvent& event) {
        {
            std::scoped_lock lock(protocolQueueMutex_);
            protocolQueue_.push_back(event);
        }
        if (event.kind != lab::core::FrameEventKind::FrameDecoded) {
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
    if (key == serialSourceKey) {
        return recordingSerial_.load();
    }
    if (key == networkSourceKey) {
        return recordingNetwork_.load();
    }
    if (key == remoteAgentSourceKey) {
        return recordingRemoteAgent_.load();
    }
    return false;
}

void SerialSession::connectSerial(lab::adapters::serial::SerialSettings settings) {
    if (recorder_.isRecording() &&
        (!recordingSerial_.load() || !sameSettings(settings, lastSettings_))) {
        emit sourceError(tr("当前 Session 未声明此串口配置，停止记录后才能更改"));
        return;
    }
    replay_.close();
    sendTarget_ = SendTarget::Serial;
    sourceManager_.close(std::string(serialSourceKey));
    lastSettings_ = std::move(settings);
    source_.setSettings(lastSettings_);
    sourceManager_.open(std::string(serialSourceKey));
}

void SerialSession::disconnectSerial() {
    sourceManager_.close(std::string(serialSourceKey));
}

void SerialSession::reconnectSerial() {
    if (lastSettings_.portName.empty()) {
        emit sourceError(tr("请先选择并连接一次目标串口"));
        return;
    }
    if (recorder_.isRecording() && !recordingSerial_.load()) {
        emit sourceError(tr("当前 Session 未声明串口源，停止记录后才能加入"));
        return;
    }
    replay_.close();
    sendTarget_ = SendTarget::Serial;
    sourceManager_.close(std::string(serialSourceKey));
    source_.setSettings(lastSettings_);
    sourceManager_.open(std::string(serialSourceKey));
}

void SerialSession::connectNetwork(lab::adapters::network::NetworkSettings settings) {
    if (recorder_.isRecording() &&
        (!recordingNetwork_.load() || !sameSettings(settings, lastNetworkSettings_))) {
        emit sourceError(tr("当前 Session 未声明此网络配置，停止记录后才能更改"));
        return;
    }
    replay_.close();
    sendTarget_ = SendTarget::Network;
    sourceManager_.close(std::string(networkSourceKey));
    lastNetworkSettings_ = std::move(settings);
    networkConfigured_ = true;
    network_.setSettings(lastNetworkSettings_);
    sourceManager_.open(std::string(networkSourceKey));
}

void SerialSession::disconnectNetwork() {
    sourceManager_.close(std::string(networkSourceKey));
}

void SerialSession::reconnectNetwork() {
    if (!networkConfigured_) {
        emit sourceError(tr("请先配置并打开一次网络数据源"));
        return;
    }
    if (recorder_.isRecording() && !recordingNetwork_.load()) {
        emit sourceError(tr("当前 Session 未声明网络源，停止记录后才能加入"));
        return;
    }
    replay_.close();
    sendTarget_ = SendTarget::Network;
    sourceManager_.close(std::string(networkSourceKey));
    network_.setSettings(lastNetworkSettings_);
    sourceManager_.open(std::string(networkSourceKey));
}

void SerialSession::connectRemoteAgent(
    lab::adapters::remote_agent::RemoteAgentSettings settings) {
    if (recorder_.isRecording() &&
        (!recordingRemoteAgent_.load() ||
         !sameSettings(settings, lastRemoteAgentSettings_))) {
        emit sourceError(tr("当前 Session 未声明此 Remote Agent 配置，停止记录后才能更改"));
        return;
    }
    replay_.close();
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
    if (recorder_.isRecording() && !recordingRemoteAgent_.load()) {
        emit sourceError(tr("当前 Session 未声明 Remote Agent，停止记录后才能加入"));
        return;
    }
    replay_.close();
    sourceManager_.close(std::string(remoteAgentSourceKey));
    remoteAgent_.setSettings(lastRemoteAgentSettings_);
    sourceManager_.open(std::string(remoteAgentSourceKey));
}

void SerialSession::setSendTarget(int target) {
    if (target == static_cast<int>(SendTarget::Serial)) {
        sendTarget_ = SendTarget::Serial;
    } else if (target == static_cast<int>(SendTarget::Network)) {
        sendTarget_ = SendTarget::Network;
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
    }
}

void SerialSession::unsubscribeRemoteTopic(
    lab::core::agent::SubscriptionRequest request) {
    if (!remoteAgent_.unsubscribe(request)) {
        emit sourceError(tr("Remote Agent 取消订阅请求未被发送"));
    }
}

void SerialSession::sendBytes(const QByteArray& bytes) {
    const auto first = reinterpret_cast<const std::uint8_t*>(bytes.constData());
    const auto data = std::span(first, static_cast<std::size_t>(bytes.size()));
    const auto key = sendTarget_ == SendTarget::Network
                         ? std::string(networkSourceKey)
                         : std::string(serialSourceKey);
    const auto accepted = sourceManager_.write(key, data);
    if (!accepted) {
        emit sourceError(tr("发送失败：当前数据源未连接或写入未被接受"));
    }
}

void SerialSession::setCsvFields(const QStringList& fields) {
    std::vector<std::string> names;
    names.reserve(fields.size());
    for (const auto& field : fields) {
        names.push_back(field.trimmed().toStdString());
    }
    activeCsvFields_ = names;
    processing_.setFieldNames(std::move(names));
    timeSeries_.clear();
    {
        std::scoped_lock lock(liveFieldsMutex_);
        liveFieldNames_.clear();
    }
}

void SerialSession::loadProtocolFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        emit protocolLoadFailed({tr("无法打开协议文件：%1").arg(file.errorString())});
        return;
    }

    const auto data = file.readAll();
    const auto result = lab::core::loadProtocolJson(
        std::string_view(data.constData(), static_cast<std::size_t>(data.size())));
    if (!result.success()) {
        QStringList issues;
        issues.reserve(static_cast<qsizetype>(result.issues.size()));
        for (const auto& issue : result.issues) {
            issues.push_back(QStringLiteral("[%1] %2")
                                 .arg(QString::fromStdString(issue.code),
                                      QString::fromStdString(issue.message)));
        }
        emit protocolLoadFailed(issues);
        return;
    }

    QStringList numericFields;
    for (const auto& field : result.definition->fields) {
        if (field.type != lab::core::FieldType::ByteArray) {
            numericFields.push_back(QString::fromStdString(field.name));
        }
    }

    const auto protocolName = QString::fromStdString(result.definition->name);
    processing_.setProtocolDefinition(*result.definition);
    timeSeries_.clear();
    {
        std::scoped_lock lock(liveFieldsMutex_);
        liveFieldNames_.clear();
    }
    {
        std::scoped_lock lock(protocolQueueMutex_);
        protocolQueue_.clear();
    }
    activeProtocolName_ = result.definition->name;
    activeProtocolJson_.assign(data.constData(), static_cast<std::size_t>(data.size()));
    recorder_.updateProtocolSnapshot(
        activeProtocolName_, activeProtocolJson_, lab::core::nowTimestampNs());
    recorder_.enqueueEvent({lab::core::nowTimestampNs(),
                            "lab_debugger",
                            "info",
                            "protocol",
                            "Protocol loaded: " + activeProtocolName_,
                            0});
    emit protocolLoaded(protocolName, numericFields);
}

void SerialSession::clearProtocol() {
    processing_.clearProtocolDefinition();
    {
        std::scoped_lock lock(protocolQueueMutex_);
        protocolQueue_.clear();
    }
    activeProtocolName_.clear();
    activeProtocolJson_.clear();
    recorder_.enqueueEvent({lab::core::nowTimestampNs(),
                            "lab_debugger",
                            "info",
                            "protocol",
                            "Protocol disabled",
                            0});
    emit protocolCleared();
}

bool SerialSession::startSession(const QString& directory) {
    if (replay_.isOpen()) {
        emit recordingChanged(false, tr("回放模式下不能开始新的实时 Session 记录"));
        return false;
    }
    const auto serialOpen = sourceManager_.isOpen(std::string(serialSourceKey));
    const auto networkOpen = sourceManager_.isOpen(std::string(networkSourceKey));
    const auto remoteAgentOpen = sourceManager_.isOpen(std::string(remoteAgentSourceKey));
    if (!serialOpen && !networkOpen && !remoteAgentOpen) {
        emit recordingChanged(false, tr("请先连接至少一个实时数据源"));
        return false;
    }
    lab::core::SessionStartOptions options;
    options.softwareVersion = "0.15.0";
    options.sessionName = QFileInfo(directory).fileName().toStdString();
    options.machineName = QSysInfo::machineHostName().toStdString();
    options.operatingSystem = QSysInfo::prettyProductName().toStdString();
    options.protocolName = activeProtocolName_;
    options.protocolJson = activeProtocolJson_;
    options.csvFields = activeCsvFields_;
    if (serialOpen) {
        options.sources.push_back({
            source_.sourceId(),
            "serial",
            lastSettings_.portName,
            {{"port", lastSettings_.portName},
             {"baud_rate", std::to_string(lastSettings_.baudRate)},
             {"data_bits", std::to_string(lastSettings_.dataBits)},
             {"stop_bits", std::to_string(static_cast<int>(lastSettings_.stopBits))},
             {"parity", std::to_string(static_cast<int>(lastSettings_.parity))},
             {"flow_control", std::to_string(static_cast<int>(lastSettings_.flowControl))}}});
    }
    if (networkOpen) {
        options.sources.push_back({
            network_.sourceId(),
            lab::adapters::network::toString(lastNetworkSettings_.mode),
            network_.sourceId(),
            {{"remote_host", lastNetworkSettings_.remoteHost},
             {"remote_port", std::to_string(lastNetworkSettings_.remotePort)},
             {"bind_address", lastNetworkSettings_.bindAddress},
             {"local_port", std::to_string(lastNetworkSettings_.localPort)}}});
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
              {"clock_sync_interval_ms", "2000"}}});
    }

    const auto sourceCount = options.sources.size();
    bool result = false;
    {
        std::scoped_lock routeLock(routingMutex_);
        processing_.flush();
        recordingSerial_.store(serialOpen);
        recordingNetwork_.store(networkOpen);
        recordingRemoteAgent_.store(remoteAgentOpen);
        result = recorder_.start(
            std::filesystem::path(directory.toStdWString()), std::move(options));
        if (!result) {
            recordingSerial_.store(false);
            recordingNetwork_.store(false);
            recordingRemoteAgent_.store(false);
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
        recordingSerial_.store(false);
        recordingNetwork_.store(false);
        recordingRemoteAgent_.store(false);
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

    sourceManager_.closeAll();
    replay_.close();
    {
        std::scoped_lock routeLock(routingMutex_);
        processing_.flush();
        processing_.resetParsers();
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
    QFile metadataFile(QDir(sessionDirectory).filePath(QStringLiteral("metadata.json")));
    if (metadataFile.open(QIODevice::ReadOnly)) {
        const auto document = QJsonDocument::fromJson(metadataFile.readAll());
        const auto replayMode = document.isObject()
                                    ? document.object()
                                          .value(QStringLiteral("replay_mode"))
                                          .toString()
                                    : QString{};
        rawOnly = replayMode == QStringLiteral("raw-only");
        structuredRosbag = replayMode == QStringLiteral("rosbag2-structured");
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
    replay_.setPath(std::filesystem::path(rawPath.toStdWString()));
    if (!replay_.open()) {
        replayRawOnly_.store(false);
        replayStructuredRosbag_.store(false);
        {
            std::scoped_lock routeLock(routingMutex_);
            rosbagReplayTopics_.clear();
        }
        emit replayOpenFailed(tr("无法打开回放文件；详细原因已写入状态栏和日志"));
        return false;
    }
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
            options.softwareVersion = "0.15.0";
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
    replayRawOnly_.store(false);
    replayStructuredRosbag_.store(false);
    std::scoped_lock routeLock(routingMutex_);
    rosbagReplayTopics_.clear();
    replayFieldNames_.clear();
    replayMappingWarnings_.clear();
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

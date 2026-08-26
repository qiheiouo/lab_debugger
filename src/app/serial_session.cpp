#include "app/serial_session.hpp"

#include "lab/core/protocol_json_loader.hpp"

#include <QFile>
#include <QMetaObject>
#include <QVariantMap>

#include <algorithm>
#include <filesystem>
#include <string_view>

namespace lab::app {

SerialSession::SerialSession(QObject* parent) : QObject(parent) {
    source_.setCallbacks({
        [this](const lab::core::DataChunk& chunk) {
            recorder_.enqueue(chunk);
            if (chunk.direction == lab::core::Direction::Rx) {
                processing_.push(chunk);
            }
            std::scoped_lock lock(uiQueueMutex_);
            uiQueue_.push_back(chunk);
        },
        [this](lab::core::SourceState state) {
            QMetaObject::invokeMethod(
                this,
                [this, state] { emit sourceStateChanged(static_cast<int>(state)); },
                Qt::QueuedConnection);
        },
        [this](const std::string& message) {
            QMetaObject::invokeMethod(
                this,
                [this, message] { emit sourceError(QString::fromStdString(message)); },
                Qt::QueuedConnection);
        }});

    processing_.setFrameHandler([this](const lab::core::FrameEvent& event) {
        std::scoped_lock lock(protocolQueueMutex_);
        protocolQueue_.push_back(event);
    });

    refreshTimer_.setInterval(33);
    refreshTimer_.setTimerType(Qt::PreciseTimer);
    connect(&refreshTimer_, &QTimer::timeout, this, &SerialSession::drainUiQueue);
    refreshTimer_.start();
}

SerialSession::~SerialSession() {
    source_.close();
    recorder_.stop();
}

std::vector<lab::adapters::serial::PortInfo> SerialSession::availablePorts() {
    return lab::adapters::serial::SerialSource::availablePorts();
}

const lab::core::TimeSeriesStore& SerialSession::timeSeries() const noexcept {
    return timeSeries_;
}

void SerialSession::connectSerial(lab::adapters::serial::SerialSettings settings) {
    lastSettings_ = std::move(settings);
    source_.setSettings(lastSettings_);
    source_.open();
}

void SerialSession::disconnectSerial() {
    source_.close();
}

void SerialSession::reconnectSerial() {
    source_.close();
    source_.setSettings(lastSettings_);
    source_.open();
}

void SerialSession::sendBytes(const QByteArray& bytes) {
    const auto first = reinterpret_cast<const std::uint8_t*>(bytes.constData());
    if (!source_.write(std::span(first, static_cast<std::size_t>(bytes.size())))) {
        emit sourceError(tr("发送失败：串口未连接或写入未被接受"));
    }
}

void SerialSession::setCsvFields(const QStringList& fields) {
    std::vector<std::string> names;
    names.reserve(fields.size());
    for (const auto& field : fields) {
        names.push_back(field.trimmed().toStdString());
    }
    processing_.setFieldNames(std::move(names));
    timeSeries_.clear();
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
        std::scoped_lock lock(protocolQueueMutex_);
        protocolQueue_.clear();
    }
    emit protocolLoaded(protocolName, numericFields);
}

void SerialSession::clearProtocol() {
    processing_.clearProtocolDefinition();
    {
        std::scoped_lock lock(protocolQueueMutex_);
        protocolQueue_.clear();
    }
    emit protocolCleared();
}

bool SerialSession::startRecording(const QString& path) {
    const auto result = recorder_.start(std::filesystem::path(path.toStdWString()));
    emit recordingChanged(result);
    return result;
}

void SerialSession::stopRecording() {
    recorder_.stop();
    emit recordingChanged(false);
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
            chunk.receiveTimestamp);
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
            item.insert(QStringLiteral("timestampNs"), event.sourceTimestamp);
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
    const auto stats = source_.statistics();
    emit statisticsChanged(
        stats.receivedBytes,
        stats.transmittedBytes,
        static_cast<qsizetype>(processing_.pendingChunks()));
}

}  // namespace lab::app

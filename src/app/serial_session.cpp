#include "app/serial_session.hpp"

#include <QMetaObject>

#include <algorithm>
#include <filesystem>

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
    const auto stats = source_.statistics();
    emit statisticsChanged(
        stats.receivedBytes,
        stats.transmittedBytes,
        static_cast<qsizetype>(processing_.pendingChunks()));
}

}  // namespace lab::app

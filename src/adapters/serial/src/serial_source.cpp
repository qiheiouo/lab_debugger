#include "lab/adapters/serial/serial_source.hpp"

#include "lab/core/logger.hpp"
#include "lab/core/timestamp.hpp"

#include <QByteArray>
#include <QMetaObject>
#include <QSerialPort>
#include <QSerialPortInfo>

#include <algorithm>
#include <functional>
#include <utility>

namespace lab::adapters::serial {
namespace {

QSerialPort::Parity toQtParity(Parity value) {
    switch (value) {
    case Parity::Even:
        return QSerialPort::EvenParity;
    case Parity::Odd:
        return QSerialPort::OddParity;
    case Parity::Space:
        return QSerialPort::SpaceParity;
    case Parity::Mark:
        return QSerialPort::MarkParity;
    case Parity::None:
    default:
        return QSerialPort::NoParity;
    }
}

QSerialPort::StopBits toQtStopBits(StopBits value) {
    switch (value) {
    case StopBits::OneAndHalf:
        return QSerialPort::OneAndHalfStop;
    case StopBits::Two:
        return QSerialPort::TwoStop;
    case StopBits::One:
    default:
        return QSerialPort::OneStop;
    }
}

QSerialPort::FlowControl toQtFlowControl(FlowControl value) {
    switch (value) {
    case FlowControl::Hardware:
        return QSerialPort::HardwareControl;
    case FlowControl::Software:
        return QSerialPort::SoftwareControl;
    case FlowControl::None:
    default:
        return QSerialPort::NoFlowControl;
    }
}

QSerialPort::DataBits toQtDataBits(std::int32_t value) {
    switch (value) {
    case 5:
        return QSerialPort::Data5;
    case 6:
        return QSerialPort::Data6;
    case 7:
        return QSerialPort::Data7;
    case 8:
    default:
        return QSerialPort::Data8;
    }
}

}  // namespace

class SerialWorker final : public QObject {
public:
    using DataHandler = std::function<void(std::vector<std::uint8_t>, lab::core::Direction)>;
    using StateHandler = std::function<void(lab::core::SourceState)>;
    using ErrorHandler = std::function<void(std::string)>;

    SerialWorker(DataHandler onData, StateHandler onState, ErrorHandler onError)
        : port_(new QSerialPort(this)),
          onData_(std::move(onData)),
          onState_(std::move(onState)),
          onError_(std::move(onError)) {
        QObject::connect(port_, &QSerialPort::readyRead, this, [this] {
            const auto bytes = port_->readAll();
            if (!bytes.isEmpty()) {
                onData_(
                    std::vector<std::uint8_t>(bytes.begin(), bytes.end()),
                    lab::core::Direction::Rx);
            }
        });
        QObject::connect(
            port_,
            &QSerialPort::errorOccurred,
            this,
            [this](QSerialPort::SerialPortError error) {
                if (error == QSerialPort::NoError || error == QSerialPort::NotOpenError) {
                    return;
                }
                onError_(port_->errorString().toStdString());
                if (error == QSerialPort::ResourceError) {
                    port_->close();
                    onState_(lab::core::SourceState::Error);
                }
            });
    }

    bool openPort(const SerialSettings& settings) {
        if (port_->isOpen()) {
            port_->close();
        }
        port_->setPortName(QString::fromStdString(settings.portName));
        port_->setBaudRate(settings.baudRate);
        port_->setDataBits(toQtDataBits(settings.dataBits));
        port_->setStopBits(toQtStopBits(settings.stopBits));
        port_->setParity(toQtParity(settings.parity));
        port_->setFlowControl(toQtFlowControl(settings.flowControl));
        if (!port_->open(QIODevice::ReadWrite)) {
            onError_(port_->errorString().toStdString());
            onState_(lab::core::SourceState::Error);
            return false;
        }
        onState_(lab::core::SourceState::Open);
        return true;
    }

    void closePort() {
        if (!port_->isOpen()) {
            onState_(lab::core::SourceState::Closed);
            return;
        }
        onState_(lab::core::SourceState::Closing);
        port_->flush();
        port_->close();
        onState_(lab::core::SourceState::Closed);
    }

    bool writeBytes(const QByteArray& bytes) {
        if (!port_->isOpen()) {
            onError_("Serial port is not open");
            return false;
        }
        const auto accepted = port_->write(bytes);
        if (accepted < 0) {
            onError_(port_->errorString().toStdString());
            return false;
        }
        if (accepted > 0) {
            const auto first = reinterpret_cast<const std::uint8_t*>(bytes.constData());
            onData_(
                std::vector<std::uint8_t>(first, first + accepted),
                lab::core::Direction::Tx);
        }
        return accepted == bytes.size();
    }

private:
    QSerialPort* port_;
    DataHandler onData_;
    StateHandler onState_;
    ErrorHandler onError_;
};

SerialSource::SerialSource() {
    worker_ = new SerialWorker(
        [this](std::vector<std::uint8_t> data, lab::core::Direction direction) {
            handleData(std::move(data), direction);
        },
        [this](lab::core::SourceState state) {
            open_.store(state == lab::core::SourceState::Open);
            publishState(state);
        },
        [this](std::string message) {
            errors_.fetch_add(1);
            lab::core::Logger::instance().log(
                lab::core::LogLevel::Error, "Serial", message);
            publishError(message);
        });
    worker_->moveToThread(&ioThread_);
    QObject::connect(&ioThread_, &QThread::finished, worker_, &QObject::deleteLater);
    ioThread_.setObjectName(QStringLiteral("Serial I/O"));
    ioThread_.start();
}

SerialSource::~SerialSource() {
    if (ioThread_.isRunning()) {
        close();
        ioThread_.quit();
        ioThread_.wait();
    }
    worker_ = nullptr;
}

void SerialSource::setSettings(SerialSettings settings) {
    std::scoped_lock lock(settingsMutex_);
    settings_ = std::move(settings);
}

SerialSettings SerialSource::settings() const {
    std::scoped_lock lock(settingsMutex_);
    return settings_;
}

std::vector<PortInfo> SerialSource::availablePorts() {
    std::vector<PortInfo> result;
    for (const auto& port : QSerialPortInfo::availablePorts()) {
        result.push_back({
            port.portName().toStdString(),
            port.description().toStdString(),
            port.manufacturer().toStdString(),
            port.serialNumber().toStdString()});
    }
    std::sort(result.begin(), result.end(), [](const PortInfo& left, const PortInfo& right) {
        return left.name < right.name;
    });
    return result;
}

bool SerialSource::open() {
    if (!ioThread_.isRunning()) {
        return false;
    }
    publishState(lab::core::SourceState::Opening);
    const auto configuration = settings();
    bool result = false;
    QMetaObject::invokeMethod(
        worker_,
        [this, configuration, &result] { result = worker_->openPort(configuration); },
        Qt::BlockingQueuedConnection);
    if (result) {
        lab::core::Logger::instance().log(
            lab::core::LogLevel::Info,
            "Serial",
            "Connected " + configuration.portName + " at " +
                std::to_string(configuration.baudRate));
    }
    return result;
}

void SerialSource::close() {
    if (!ioThread_.isRunning()) {
        return;
    }
    QMetaObject::invokeMethod(
        worker_, [this] { worker_->closePort(); }, Qt::BlockingQueuedConnection);
}

bool SerialSource::isOpen() const noexcept {
    return open_.load();
}

bool SerialSource::write(std::span<const std::uint8_t> data) {
    if (!isOpen() || data.empty()) {
        return false;
    }
    const QByteArray bytes(
        reinterpret_cast<const char*>(data.data()), static_cast<qsizetype>(data.size()));
    bool result = false;
    QMetaObject::invokeMethod(
        worker_,
        [this, bytes, &result] { result = worker_->writeBytes(bytes); },
        Qt::BlockingQueuedConnection);
    return result;
}

std::string SerialSource::sourceId() const {
    const auto configuration = settings();
    return "serial:" + configuration.portName;
}

lab::core::SourceStatistics SerialSource::statistics() const noexcept {
    return {
        receivedBytes_.load(),
        transmittedBytes_.load(),
        receivedChunks_.load(),
        transmittedChunks_.load(),
        errors_.load()};
}

void SerialSource::handleData(
    std::vector<std::uint8_t> payload,
    lab::core::Direction direction) {
    if (direction == lab::core::Direction::Rx) {
        receivedBytes_.fetch_add(payload.size());
        receivedChunks_.fetch_add(1);
    } else {
        transmittedBytes_.fetch_add(payload.size());
        transmittedChunks_.fetch_add(1);
    }
    const auto now = lab::core::nowTimestampNs();
    publishData({
        sourceId(),
        now,
        now,
        sequence_.fetch_add(1),
        direction,
        std::move(payload)});
}

}  // namespace lab::adapters::serial


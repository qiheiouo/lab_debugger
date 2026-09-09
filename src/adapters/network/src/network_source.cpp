#include "lab/adapters/network/network_source.hpp"

#include "lab/core/logger.hpp"
#include "lab/core/timestamp.hpp"

#include <QAbstractSocket>
#include <QByteArray>
#include <QHostAddress>
#include <QMetaObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>

#include <functional>
#include <utility>

namespace lab::adapters::network {
namespace {

QHostAddress bindAddress(const std::string& value) {
    if (value.empty() || value == "0.0.0.0" || value == "*") {
        return QHostAddress::AnyIPv4;
    }
    return QHostAddress(QString::fromStdString(value));
}

}  // namespace

class NetworkWorker final : public QObject {
public:
    using DataHandler = std::function<void(std::vector<std::uint8_t>, lab::core::Direction)>;
    using StateHandler = std::function<void(lab::core::SourceState)>;
    using ErrorHandler = std::function<void(std::string)>;

    NetworkWorker(DataHandler onData, StateHandler onState, ErrorHandler onError)
        : onData_(std::move(onData)),
          onState_(std::move(onState)),
          onError_(std::move(onError)) {}

    bool openEndpoint(NetworkSettings settings) {
        closeEndpoint(false);
        settings_ = std::move(settings);
        switch (settings_.mode) {
        case NetworkMode::TcpClient:
            return openTcpClient();
        case NetworkMode::TcpServer:
            return openTcpServer();
        case NetworkMode::Udp:
            return openUdp();
        }
        return false;
    }

    void closeEndpoint(bool announce = true) {
        if (announce) {
            onState_(lab::core::SourceState::Closing);
        }
        if (tcpClient_) {
            tcpClient_->abort();
            delete tcpClient_;
            tcpClient_ = nullptr;
        }
        if (tcpPeer_) {
            tcpPeer_->abort();
            delete tcpPeer_;
            tcpPeer_ = nullptr;
        }
        if (tcpServer_) {
            tcpServer_->close();
            delete tcpServer_;
            tcpServer_ = nullptr;
        }
        if (udp_) {
            udp_->close();
            delete udp_;
            udp_ = nullptr;
        }
        if (announce) {
            onState_(lab::core::SourceState::Closed);
        }
    }

    bool writeBytes(const QByteArray& bytes) {
        if (settings_.mode == NetworkMode::Udp) {
            if (!udp_) {
                onError_("UDP endpoint is not open");
                return false;
            }
            const QHostAddress destination(QString::fromStdString(settings_.remoteHost));
            if (destination.isNull() || settings_.remotePort == 0) {
                onError_("UDP remote address or port is invalid");
                return false;
            }
            const auto accepted = udp_->writeDatagram(bytes, destination, settings_.remotePort);
            if (accepted < 0) {
                onError_(udp_->errorString().toStdString());
                return false;
            }
            publishTx(bytes, accepted);
            return accepted == bytes.size();
        }

        auto* socket = settings_.mode == NetworkMode::TcpClient ? tcpClient_ : tcpPeer_;
        if (!socket || socket->state() != QAbstractSocket::ConnectedState) {
            onError_(settings_.mode == NetworkMode::TcpServer
                         ? "TCP server has no connected client"
                         : "TCP client is not connected");
            return false;
        }
        const auto accepted = socket->write(bytes);
        if (accepted < 0) {
            onError_(socket->errorString().toStdString());
            return false;
        }
        publishTx(bytes, accepted);
        return accepted == bytes.size();
    }

private:
    bool openTcpClient() {
        if (settings_.remoteHost.empty() || settings_.remotePort == 0) {
            onError_("TCP remote host or port is invalid");
            onState_(lab::core::SourceState::Error);
            return false;
        }
        tcpClient_ = new QTcpSocket(this);
        connectTcpSocket(tcpClient_, true);
        tcpClient_->connectToHost(
            QString::fromStdString(settings_.remoteHost), settings_.remotePort);
        return true;
    }

    bool openTcpServer() {
        if (settings_.localPort == 0) {
            onError_("TCP listen port is invalid");
            onState_(lab::core::SourceState::Error);
            return false;
        }
        const auto address = bindAddress(settings_.bindAddress);
        if (address.isNull()) {
            onError_("TCP bind address is invalid");
            onState_(lab::core::SourceState::Error);
            return false;
        }
        tcpServer_ = new QTcpServer(this);
        QObject::connect(tcpServer_, &QTcpServer::newConnection, this, [this] {
            while (tcpServer_ && tcpServer_->hasPendingConnections()) {
                auto* incoming = tcpServer_->nextPendingConnection();
                if (tcpPeer_) {
                    tcpPeer_->abort();
                    delete tcpPeer_;
                }
                tcpPeer_ = incoming;
                tcpPeer_->setParent(this);
                connectTcpSocket(tcpPeer_, false);
            }
        });
        if (!tcpServer_->listen(address, settings_.localPort)) {
            onError_(tcpServer_->errorString().toStdString());
            delete tcpServer_;
            tcpServer_ = nullptr;
            onState_(lab::core::SourceState::Error);
            return false;
        }
        onState_(lab::core::SourceState::Open);
        return true;
    }

    bool openUdp() {
        if (settings_.localPort == 0) {
            onError_("UDP local port is invalid");
            onState_(lab::core::SourceState::Error);
            return false;
        }
        const auto address = bindAddress(settings_.bindAddress);
        if (address.isNull()) {
            onError_("UDP bind address is invalid");
            onState_(lab::core::SourceState::Error);
            return false;
        }
        udp_ = new QUdpSocket(this);
        QObject::connect(udp_, &QUdpSocket::readyRead, this, [this] {
            while (udp_ && udp_->hasPendingDatagrams()) {
                const auto size = udp_->pendingDatagramSize();
                if (size < 0) {
                    onError_(udp_->errorString().toStdString());
                    return;
                }
                QByteArray bytes(static_cast<qsizetype>(size), Qt::Uninitialized);
                const auto received = udp_->readDatagram(bytes.data(), bytes.size());
                if (received < 0) {
                    onError_(udp_->errorString().toStdString());
                    return;
                }
                const auto first = reinterpret_cast<const std::uint8_t*>(bytes.constData());
                onData_(std::vector<std::uint8_t>(first, first + received),
                        lab::core::Direction::Rx);
            }
        });
        if (!udp_->bind(address, settings_.localPort,
                        QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
            onError_(udp_->errorString().toStdString());
            delete udp_;
            udp_ = nullptr;
            onState_(lab::core::SourceState::Error);
            return false;
        }
        onState_(lab::core::SourceState::Open);
        return true;
    }

    void connectTcpSocket(QTcpSocket* socket, bool publishConnectionState) {
        QObject::connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
            const auto bytes = socket->readAll();
            if (!bytes.isEmpty()) {
                onData_(std::vector<std::uint8_t>(bytes.begin(), bytes.end()),
                        lab::core::Direction::Rx);
            }
        });
        QObject::connect(socket, &QTcpSocket::connected, this, [this, publishConnectionState] {
            if (publishConnectionState) {
                onState_(lab::core::SourceState::Open);
            }
        });
        QObject::connect(socket, &QTcpSocket::disconnected, this, [this, socket, publishConnectionState] {
            if (socket == tcpPeer_) {
                tcpPeer_ = nullptr;
                socket->deleteLater();
            }
            if (publishConnectionState) {
                onState_(lab::core::SourceState::Closed);
            }
        });
        QObject::connect(
            socket,
            &QTcpSocket::errorOccurred,
            this,
            [this, publishConnectionState](QAbstractSocket::SocketError error) {
                if (error == QAbstractSocket::RemoteHostClosedError) {
                    return;
                }
                auto* socket = qobject_cast<QTcpSocket*>(sender());
                onError_(socket ? socket->errorString().toStdString() : "TCP socket error");
                if (publishConnectionState) {
                    onState_(lab::core::SourceState::Error);
                }
            });
    }

    void publishTx(const QByteArray& bytes, qint64 accepted) {
        if (accepted <= 0) {
            return;
        }
        const auto first = reinterpret_cast<const std::uint8_t*>(bytes.constData());
        onData_(std::vector<std::uint8_t>(first, first + accepted), lab::core::Direction::Tx);
    }

    NetworkSettings settings_;
    QTcpSocket* tcpClient_{};
    QTcpServer* tcpServer_{};
    QTcpSocket* tcpPeer_{};
    QUdpSocket* udp_{};
    DataHandler onData_;
    StateHandler onState_;
    ErrorHandler onError_;
};

NetworkSource::NetworkSource() {
    worker_ = new NetworkWorker(
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
                lab::core::LogLevel::Error, "Network", message);
            publishError(message);
        });
    worker_->moveToThread(&ioThread_);
    QObject::connect(&ioThread_, &QThread::finished, worker_, &QObject::deleteLater);
    ioThread_.setObjectName(QStringLiteral("Network I/O"));
    ioThread_.start();
}

NetworkSource::~NetworkSource() {
    if (ioThread_.isRunning()) {
        close();
        ioThread_.quit();
        ioThread_.wait();
    }
    worker_ = nullptr;
}

void NetworkSource::setSettings(NetworkSettings settings) {
    std::scoped_lock lock(settingsMutex_);
    settings_ = std::move(settings);
}

NetworkSettings NetworkSource::settings() const {
    std::scoped_lock lock(settingsMutex_);
    return settings_;
}

bool NetworkSource::open() {
    if (!ioThread_.isRunning()) {
        return false;
    }
    publishState(lab::core::SourceState::Opening);
    const auto configuration = settings();
    endpointCreated_.store(false);
    bool accepted = false;
    QMetaObject::invokeMethod(
        worker_,
        [this, configuration, &accepted] {
            accepted = worker_->openEndpoint(configuration);
        },
        Qt::BlockingQueuedConnection);
    if (accepted) {
        endpointCreated_.store(true);
        lab::core::Logger::instance().log(
            lab::core::LogLevel::Info, "Network", "Opened " + sourceId());
    }
    return accepted;
}

void NetworkSource::close() {
    if (!ioThread_.isRunning() || !endpointCreated_.exchange(false)) {
        return;
    }
    QMetaObject::invokeMethod(
        worker_, [this] { worker_->closeEndpoint(); }, Qt::BlockingQueuedConnection);
}

bool NetworkSource::isOpen() const noexcept {
    return open_.load();
}

bool NetworkSource::write(std::span<const std::uint8_t> data) {
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

std::string NetworkSource::sourceId() const {
    return networkSourceId(settings());
}

std::string networkSourceId(const NetworkSettings& configuration) {
    switch (configuration.mode) {
    case NetworkMode::TcpClient:
        return "tcp-client:" + configuration.remoteHost + ':' +
               std::to_string(configuration.remotePort);
    case NetworkMode::TcpServer:
        return "tcp-server:" + configuration.bindAddress + ':' +
               std::to_string(configuration.localPort);
    case NetworkMode::Udp:
        return "udp:" + configuration.bindAddress + ':' +
               std::to_string(configuration.localPort);
    }
    return "network";
}

lab::core::SourceStatistics NetworkSource::statistics() const noexcept {
    return {receivedBytes_.load(),
            transmittedBytes_.load(),
            receivedChunks_.load(),
            transmittedChunks_.load(),
            errors_.load()};
}

void NetworkSource::handleData(
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
    publishData({sourceId(), now, now, sequence_.fetch_add(1), direction, std::move(payload)});
}

std::string toString(NetworkMode mode) {
    switch (mode) {
    case NetworkMode::TcpClient: return "tcp_client";
    case NetworkMode::TcpServer: return "tcp_server";
    case NetworkMode::Udp: return "udp";
    }
    return "unknown";
}

}  // namespace lab::adapters::network

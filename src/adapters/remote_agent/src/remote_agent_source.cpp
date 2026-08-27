#include "lab/adapters/remote_agent/remote_agent_source.hpp"

#include "lab/core/logger.hpp"
#include "lab/core/timestamp.hpp"

#include <QAbstractSocket>
#include <QByteArray>
#include <QMetaObject>
#include <QTcpSocket>
#include <QTimer>

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <utility>
#include <variant>

namespace lab::adapters::remote_agent {
namespace {

constexpr int handshakeTimeoutMs = 5000;
constexpr int reconnectInitialDelayMs = 250;
constexpr int reconnectMaximumDelayMs = 8000;
constexpr int clockSyncIntervalMs = 2000;
constexpr lab::core::Timestamp clockSyncResponseTimeoutNs = 10'000'000'000LL;

constexpr std::uint32_t supportedCapabilities =
    lab::core::agent::capabilityMask(lab::core::agent::Capability::TopicDiscovery) |
    lab::core::agent::capabilityMask(lab::core::agent::Capability::SerializedMessages) |
    lab::core::agent::capabilityMask(lab::core::agent::Capability::NumericFields) |
    lab::core::agent::capabilityMask(lab::core::agent::Capability::TextFields) |
    lab::core::agent::capabilityMask(lab::core::agent::Capability::GraphUpdates);

constexpr std::uint32_t sampleCapabilityMask =
    lab::core::agent::capabilityMask(lab::core::agent::Capability::SerializedMessages) |
    lab::core::agent::capabilityMask(lab::core::agent::Capability::NumericFields) |
    lab::core::agent::capabilityMask(lab::core::agent::Capability::TextFields);

std::string protocolMessage(const lab::core::agent::DecodeIssue& issue) {
    return "Remote Agent protocol: " + issue.message;
}

bool hasCapability(std::uint32_t capabilities, lab::core::agent::Capability capability) {
    return (capabilities & lab::core::agent::capabilityMask(capability)) != 0U;
}

std::string subscriptionKey(const lab::core::agent::SubscriptionRequest& request) {
    std::string key;
    key.reserve(request.topic.size() + request.type.size() + 1);
    key.append(request.topic);
    key.push_back('\0');
    key.append(request.type);
    return key;
}

}  // namespace

class RemoteAgentWorker final : public QObject {
public:
    using HelloHandler = std::function<void(const lab::core::agent::Hello&)>;
    using CatalogHandler = std::function<void(const lab::core::agent::TopicCatalog&)>;
    using SampleHandler = std::function<void(
        const lab::core::agent::Frame&,
        const lab::core::agent::SampleBatch&)>;
    using IssueHandler = std::function<void(const lab::core::agent::DecodeIssue&)>;
    using ClockHandler = std::function<void(const lab::core::ClockSyncEstimate&)>;
    using StateHandler = std::function<void(lab::core::SourceState, HandshakeState)>;
    using ErrorHandler = std::function<void(std::string)>;
    using WireHandler = std::function<void(lab::core::Direction, std::size_t)>;

    RemoteAgentWorker(
        HelloHandler onHello,
        CatalogHandler onCatalog,
        SampleHandler onSample,
        IssueHandler onIssue,
        ClockHandler onClockSync,
        StateHandler onState,
        ErrorHandler onError,
        WireHandler onWire)
        : onHello_(std::move(onHello)),
          onCatalog_(std::move(onCatalog)),
          onSample_(std::move(onSample)),
          onIssue_(std::move(onIssue)),
          onClockSync_(std::move(onClockSync)),
          onState_(std::move(onState)),
          onError_(std::move(onError)),
          onWire_(std::move(onWire)) {}

    bool openEndpoint(RemoteAgentSettings settings) {
        const auto sameEndpoint = settings_.host == settings.host &&
                                  settings_.port == settings.port;
        closeEndpoint(false);
        settings_ = std::move(settings);
        if (settings_.host.empty() || settings_.port == 0 ||
            settings_.clientName.empty() || settings_.clientVersion.empty()) {
            fail("Remote Agent address and client identity must not be empty");
            return false;
        }
        if (!sameEndpoint) {
            desiredSubscriptions_.clear();
        }
        desiredOpen_ = true;
        closing_ = false;
        reconnectAttempt_ = 0;
        handshakeTimer_ = new QTimer(this);
        handshakeTimer_->setSingleShot(true);
        QObject::connect(handshakeTimer_, &QTimer::timeout, this, [this] {
            recoverTransport(
                "Remote Agent handshake timed out after 5 seconds", true);
            if (socket_) socket_->abort();
        });
        reconnectTimer_ = new QTimer(this);
        reconnectTimer_->setSingleShot(true);
        QObject::connect(reconnectTimer_, &QTimer::timeout, this, [this] {
            startConnection();
        });
        clockSyncTimer_ = new QTimer(this);
        clockSyncTimer_->setInterval(clockSyncIntervalMs);
        QObject::connect(clockSyncTimer_, &QTimer::timeout, this, [this] {
            requestClockSync();
        });
        startConnection();
        return true;
    }

    void closeEndpoint(bool announce = true) {
        desiredOpen_ = false;
        closing_ = true;
        if (announce) {
            onState_(lab::core::SourceState::Closing, state_);
        }
        if (handshakeTimer_) {
            handshakeTimer_->stop();
            delete handshakeTimer_;
            handshakeTimer_ = nullptr;
        }
        if (reconnectTimer_) {
            reconnectTimer_->stop();
            delete reconnectTimer_;
            reconnectTimer_ = nullptr;
        }
        if (clockSyncTimer_) {
            clockSyncTimer_->stop();
            delete clockSyncTimer_;
            clockSyncTimer_ = nullptr;
        }
        if (socket_) {
            QObject::disconnect(socket_, nullptr, this, nullptr);
            socket_->abort();
            delete socket_;
            socket_ = nullptr;
        }
        decoder_.reset();
        lastInboundSequence_.reset();
        negotiatedCapabilities_ = 0;
        clockEstimator_.reset();
        pendingClockPing_.reset();
        state_ = HandshakeState::Disconnected;
        if (announce) {
            onState_(lab::core::SourceState::Closed, state_);
        }
    }

    bool requestTopicCatalog() {
        if (!hasCapability(
                negotiatedCapabilities_, lab::core::agent::Capability::TopicDiscovery)) {
            onError_("Remote Agent did not negotiate topic discovery");
            return false;
        }
        return sendReadyFrame(lab::core::agent::MessageType::TopicCatalogRequest, {});
    }

    bool subscribe(const lab::core::agent::SubscriptionRequest& request) {
        if ((negotiatedCapabilities_ & sampleCapabilityMask) == 0U) {
            onError_("Remote Agent did not negotiate any sample capability");
            return false;
        }
        try {
            const auto sent = sendReadyFrame(
                lab::core::agent::MessageType::Subscribe,
                lab::core::agent::encodeSubscriptionRequest(request));
            if (sent) desiredSubscriptions_[subscriptionKey(request)] = request;
            return sent;
        } catch (const std::exception& exception) {
            onError_(exception.what());
            return false;
        }
    }

    bool unsubscribe(const lab::core::agent::SubscriptionRequest& request) {
        if ((negotiatedCapabilities_ & sampleCapabilityMask) == 0U) {
            onError_("Remote Agent did not negotiate any sample capability");
            return false;
        }
        try {
            const auto sent = sendReadyFrame(
                lab::core::agent::MessageType::Unsubscribe,
                lab::core::agent::encodeSubscriptionRequest(request));
            if (sent) desiredSubscriptions_.erase(subscriptionKey(request));
            return sent;
        } catch (const std::exception& exception) {
            onError_(exception.what());
            return false;
        }
    }

private:
    void startConnection() {
        if (!desiredOpen_) return;
        if (socket_) {
            QObject::disconnect(socket_, nullptr, this, nullptr);
            socket_->abort();
            delete socket_;
            socket_ = nullptr;
        }
        decoder_.reset();
        lastInboundSequence_.reset();
        outboundSequence_ = 0;
        negotiatedCapabilities_ = 0;
        if (clockSyncTimer_) clockSyncTimer_->stop();
        clockEstimator_.reset();
        pendingClockPing_.reset();
        closing_ = false;
        state_ = HandshakeState::Disconnected;
        onState_(lab::core::SourceState::Opening, state_);

        socket_ = new QTcpSocket(this);
        QObject::connect(socket_, &QTcpSocket::connected, this, [this] {
            state_ = HandshakeState::AwaitingHello;
            onState_(lab::core::SourceState::Opening, state_);
            if (handshakeTimer_) handshakeTimer_->start(handshakeTimeoutMs);
        });
        QObject::connect(socket_, &QTcpSocket::readyRead, this, [this] {
            const auto bytes = socket_->readAll();
            if (bytes.isEmpty()) return;
            onWire_(lab::core::Direction::Rx, static_cast<std::size_t>(bytes.size()));
            const auto first = reinterpret_cast<const std::uint8_t*>(bytes.constData());
            const auto result = decoder_.consume(
                std::span(first, static_cast<std::size_t>(bytes.size())));
            for (const auto& issue : result.issues) {
                onIssue_(issue);
                onError_(protocolMessage(issue));
            }
            for (const auto& frame : result.frames) {
                if (!processFrame(frame)) break;
            }
        });
        QObject::connect(socket_, &QTcpSocket::disconnected, this, [this] {
            if (handshakeTimer_) handshakeTimer_->stop();
            if (closing_ || !desiredOpen_ || state_ == HandshakeState::Error ||
                (reconnectTimer_ && reconnectTimer_->isActive())) {
                return;
            }
            const auto handshakeFailure = state_ == HandshakeState::AwaitingHello;
            recoverTransport(
                handshakeFailure
                    ? "Remote Agent closed the connection before handshake completed"
                    : "Remote Agent connection closed",
                handshakeFailure);
        });
        QObject::connect(
            socket_,
            &QTcpSocket::errorOccurred,
            this,
            [this](QAbstractSocket::SocketError error) {
                if (closing_ || !desiredOpen_ || state_ == HandshakeState::Error ||
                    error == QAbstractSocket::RemoteHostClosedError) {
                    return;
                }
                recoverTransport(
                    socket_ ? socket_->errorString().toStdString()
                            : "Remote Agent socket error",
                    true);
            });
        socket_->connectToHost(QString::fromStdString(settings_.host), settings_.port);
    }

    void recoverTransport(std::string message, bool errorWithoutReconnect) {
        if (closing_ || !desiredOpen_ || state_ == HandshakeState::Error) return;
        if (clockSyncTimer_) clockSyncTimer_->stop();
        pendingClockPing_.reset();
        if (settings_.autoReconnect) {
            if (reconnectTimer_ && reconnectTimer_->isActive()) return;
            onError_(std::move(message));
            scheduleReconnect();
            return;
        }
        if (errorWithoutReconnect) {
            fail(std::move(message));
        } else {
            state_ = HandshakeState::Disconnected;
            onState_(lab::core::SourceState::Closed, state_);
        }
    }

    void scheduleReconnect() {
        if (!desiredOpen_ || !reconnectTimer_ || reconnectTimer_->isActive()) return;
        if (handshakeTimer_) handshakeTimer_->stop();
        const auto exponent = std::min(reconnectAttempt_, 5U);
        const auto delay = std::min(
            reconnectInitialDelayMs * (1 << exponent), reconnectMaximumDelayMs);
        ++reconnectAttempt_;
        state_ = HandshakeState::Disconnected;
        onState_(lab::core::SourceState::Opening, state_);
        reconnectTimer_->start(delay);
    }

    bool processFrame(const lab::core::agent::Frame& frame) {
        if (frame.flags != 0) {
            failFatal("Remote Agent sent unsupported non-zero frame flags");
            return false;
        }
        if (lastInboundSequence_ && frame.sequence <= *lastInboundSequence_) {
            failFatal("Remote Agent frame sequence is not strictly increasing");
            return false;
        }
        lastInboundSequence_ = frame.sequence;

        if (state_ == HandshakeState::AwaitingHello) {
            if (frame.type != lab::core::agent::MessageType::Hello) {
                failFatal("Remote Agent must send hello as its first frame");
                return false;
            }
            return acceptHello(frame);
        }
        if (state_ != HandshakeState::Ready) {
            return false;
        }

        std::string error;
        switch (frame.type) {
        case lab::core::agent::MessageType::TopicCatalog: {
            const auto catalog = lab::core::agent::decodeTopicCatalog(frame.payload, &error);
            if (!catalog) return malformed(frame, error);
            onCatalog_(*catalog);
            return true;
        }
        case lab::core::agent::MessageType::SampleBatch: {
            const auto sample = lab::core::agent::decodeSampleBatch(frame.payload, &error);
            if (!sample) return malformed(frame, error);
            onSample_(frame, *sample);
            return true;
        }
        case lab::core::agent::MessageType::Error: {
            const auto value = lab::core::agent::decodeAgentError(frame.payload, &error);
            if (!value) return malformed(frame, error);
            onError_("Remote Agent error " + std::to_string(value->code) + " [" +
                     value->context + "]: " + value->message);
            return true;
        }
        case lab::core::agent::MessageType::Ping: {
            const auto nonce = lab::core::agent::decodeNonce(frame.payload, &error);
            if (!nonce) return malformed(frame, error);
            return sendFrame(
                lab::core::agent::MessageType::Pong,
                lab::core::agent::encodeNonce(*nonce));
        }
        case lab::core::agent::MessageType::Pong:
            if (const auto nonce = lab::core::agent::decodeNonce(frame.payload, &error)) {
                if (pendingClockPing_ && pendingClockPing_->first == *nonce) {
                    const auto localReceive = lab::core::nowTimestampNs();
                    const auto agentTimestamp = frame.agentReceiveTimestamp != 0
                                                    ? frame.agentReceiveTimestamp
                                                    : frame.sourceTimestamp;
                    const auto estimate = clockEstimator_.addSample(
                        pendingClockPing_->second, agentTimestamp, localReceive);
                    pendingClockPing_.reset();
                    if (estimate) onClockSync_(*estimate);
                }
            } else {
                return malformed(frame, error);
            }
            return true;
        case lab::core::agent::MessageType::Hello:
        case lab::core::agent::MessageType::HelloAck:
        case lab::core::agent::MessageType::Subscribe:
        case lab::core::agent::MessageType::Unsubscribe:
        case lab::core::agent::MessageType::TopicCatalogRequest:
            failFatal("Remote Agent sent a message that is invalid in the ready state: " +
                      lab::core::agent::toString(frame.type));
            return false;
        }
        return false;
    }

    bool acceptHello(const lab::core::agent::Frame& frame) {
        std::string error;
        const auto hello = lab::core::agent::decodeHello(frame.payload, &error);
        if (!hello) {
            return malformed(frame, error);
        }
        negotiatedCapabilities_ = hello->capabilities & supportedCapabilities;
        if (!hasCapability(
                negotiatedCapabilities_,
                lab::core::agent::Capability::TopicDiscovery)) {
            negotiatedCapabilities_ &= ~lab::core::agent::capabilityMask(
                lab::core::agent::Capability::GraphUpdates);
        }
        const lab::core::agent::HelloAck ack{
            settings_.clientName,
            settings_.clientVersion,
            negotiatedCapabilities_};
        if (!sendFrame(
                lab::core::agent::MessageType::HelloAck,
                lab::core::agent::encodeHelloAck(ack))) {
            failFatal("Failed to send Remote Agent hello acknowledgement");
            return false;
        }
        if (handshakeTimer_) handshakeTimer_->stop();
        onHello_(*hello);
        state_ = HandshakeState::Ready;
        reconnectAttempt_ = 0;
        onState_(lab::core::SourceState::Open, state_);
        if (hasCapability(
                negotiatedCapabilities_,
                lab::core::agent::Capability::TopicDiscovery) &&
            !requestTopicCatalog()) {
            recoverTransport("Failed to request the initial Remote Agent topic catalog", true);
            if (socket_) socket_->abort();
            return false;
        }
        if ((negotiatedCapabilities_ & sampleCapabilityMask) == 0U) {
            if (!desiredSubscriptions_.empty()) {
                onError_(
                    "Remote Agent no longer offers sample capabilities; subscriptions "
                    "were not restored");
            }
        } else {
            for (const auto& [key, request] : desiredSubscriptions_) {
                static_cast<void>(key);
                if (!sendReadyFrame(
                        lab::core::agent::MessageType::Subscribe,
                        lab::core::agent::encodeSubscriptionRequest(request))) {
                    recoverTransport("Failed to restore Remote Agent subscriptions", true);
                    if (socket_) socket_->abort();
                    return false;
                }
            }
        }
        if (clockSyncTimer_) clockSyncTimer_->start();
        requestClockSync();
        return true;
    }

    void requestClockSync() {
        if (state_ != HandshakeState::Ready) return;
        const auto now = lab::core::nowTimestampNs();
        if (pendingClockPing_) {
            if (now >= pendingClockPing_->second &&
                now - pendingClockPing_->second <= clockSyncResponseTimeoutNs) {
                return;
            }
            pendingClockPing_.reset();
        }
        const auto nonce = nextClockNonce_++;
        lab::core::Timestamp sentAt{};
        if (sendFrame(
                lab::core::agent::MessageType::Ping,
                lab::core::agent::encodeNonce(nonce),
                &sentAt)) {
            pendingClockPing_ = std::pair{nonce, sentAt};
        }
    }

    bool malformed(const lab::core::agent::Frame& frame, const std::string& error) {
        failFatal("Malformed " + lab::core::agent::toString(frame.type) +
                  " payload: " + error);
        return false;
    }

    bool sendReadyFrame(
        lab::core::agent::MessageType type,
        std::vector<std::uint8_t> payload) {
        if (state_ != HandshakeState::Ready) {
            onError_("Remote Agent handshake is not ready");
            return false;
        }
        return sendFrame(type, std::move(payload));
    }

    bool sendFrame(
        lab::core::agent::MessageType type,
        std::vector<std::uint8_t> payload,
        lab::core::Timestamp* sentAt = nullptr) {
        if (!socket_ || socket_->state() != QAbstractSocket::ConnectedState) {
            onError_("Remote Agent socket is not connected");
            return false;
        }
        const auto now = lab::core::nowTimestampNs();
        const lab::core::agent::Frame frame{
            type, 0, outboundSequence_++, now, now, std::move(payload)};
        const auto encoded = lab::core::agent::encodeFrame(frame);
        const QByteArray bytes(
            reinterpret_cast<const char*>(encoded.data()),
            static_cast<qsizetype>(encoded.size()));
        const auto accepted = socket_->write(bytes);
        if (accepted < 0) {
            onError_(socket_->errorString().toStdString());
            return false;
        }
        if (accepted > 0) {
            onWire_(lab::core::Direction::Tx, static_cast<std::size_t>(accepted));
        }
        if (accepted != bytes.size()) {
            onError_("Remote Agent socket accepted only part of a control frame");
            return false;
        }
        if (sentAt) *sentAt = now;
        return true;
    }

    void fail(std::string message) {
        state_ = HandshakeState::Error;
        onError_(std::move(message));
        onState_(lab::core::SourceState::Error, state_);
    }

    void failFatal(std::string message) {
        fail(std::move(message));
        if (handshakeTimer_) {
            handshakeTimer_->stop();
        }
        if (reconnectTimer_) reconnectTimer_->stop();
        if (clockSyncTimer_) clockSyncTimer_->stop();
        pendingClockPing_.reset();
        if (socket_) {
            socket_->abort();
        }
    }

    RemoteAgentSettings settings_;
    QTcpSocket* socket_{};
    QTimer* handshakeTimer_{};
    QTimer* reconnectTimer_{};
    QTimer* clockSyncTimer_{};
    lab::core::agent::StreamDecoder decoder_;
    std::optional<std::uint64_t> lastInboundSequence_;
    std::uint64_t outboundSequence_{};
    std::uint32_t negotiatedCapabilities_{};
    lab::core::ClockSyncEstimator clockEstimator_;
    std::optional<std::pair<std::uint64_t, lab::core::Timestamp>> pendingClockPing_;
    std::uint64_t nextClockNonce_{1};
    std::unordered_map<std::string, lab::core::agent::SubscriptionRequest>
        desiredSubscriptions_;
    unsigned int reconnectAttempt_{};
    HandshakeState state_{HandshakeState::Disconnected};
    bool closing_{};
    bool desiredOpen_{};
    HelloHandler onHello_;
    CatalogHandler onCatalog_;
    SampleHandler onSample_;
    IssueHandler onIssue_;
    ClockHandler onClockSync_;
    StateHandler onState_;
    ErrorHandler onError_;
    WireHandler onWire_;
};

RemoteAgentSource::RemoteAgentSource() {
    worker_ = new RemoteAgentWorker(
        [this](const lab::core::agent::Hello& hello) { handleHello(hello); },
        [this](const lab::core::agent::TopicCatalog& catalog) {
            const auto callbacks = agentCallbacks();
            if (callbacks.onTopicCatalog) callbacks.onTopicCatalog(catalog);
        },
        [this](const lab::core::agent::Frame& frame,
               const lab::core::agent::SampleBatch& batch) {
            handleSample(frame, batch);
        },
        [this](const lab::core::agent::DecodeIssue& issue) {
            handleProtocolIssue(issue);
        },
        [this](const lab::core::ClockSyncEstimate& estimate) {
            handleClockSync(estimate);
        },
        [this](lab::core::SourceState state, HandshakeState handshake) {
            open_.store(state == lab::core::SourceState::Open);
            handshakeState_.store(handshake);
            if (state != lab::core::SourceState::Open) {
                std::scoped_lock lock(clockSyncMutex_);
                clockSyncEstimate_.reset();
            }
            publishState(state);
        },
        [this](std::string message) {
            errors_.fetch_add(1);
            lab::core::Logger::instance().log(
                lab::core::LogLevel::Error, "RemoteAgent", message);
            publishError(message);
        },
        [this](lab::core::Direction direction, std::size_t count) {
            handleWireBytes(direction, count);
        });
    worker_->moveToThread(&ioThread_);
    QObject::connect(&ioThread_, &QThread::finished, worker_, &QObject::deleteLater);
    ioThread_.setObjectName(QStringLiteral("Remote Agent I/O"));
    ioThread_.start();
}

RemoteAgentSource::~RemoteAgentSource() {
    if (ioThread_.isRunning()) {
        close();
        ioThread_.quit();
        ioThread_.wait();
    }
    worker_ = nullptr;
}

void RemoteAgentSource::setSettings(RemoteAgentSettings settings) {
    std::scoped_lock lock(settingsMutex_);
    settings_ = std::move(settings);
}

RemoteAgentSettings RemoteAgentSource::settings() const {
    std::scoped_lock lock(settingsMutex_);
    return settings_;
}

void RemoteAgentSource::setAgentCallbacks(RemoteAgentCallbacks callbacks) {
    std::scoped_lock lock(agentCallbackMutex_);
    agentCallbacks_ = std::move(callbacks);
}

bool RemoteAgentSource::open() {
    if (!ioThread_.isRunning()) {
        return false;
    }
    publishState(lab::core::SourceState::Opening);
    const auto configuration = settings();
    endpointCreated_.store(false);
    {
        std::scoped_lock lock(agentIdentityMutex_);
        agentId_.clear();
    }
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
            lab::core::LogLevel::Info, "RemoteAgent", "Connecting to " + sourceId());
    }
    return accepted;
}

void RemoteAgentSource::close() {
    if (!ioThread_.isRunning() || !endpointCreated_.exchange(false)) {
        return;
    }
    QMetaObject::invokeMethod(
        worker_, [this] { worker_->closeEndpoint(); }, Qt::BlockingQueuedConnection);
}

bool RemoteAgentSource::isOpen() const noexcept {
    return open_.load();
}

bool RemoteAgentSource::write(std::span<const std::uint8_t> data) {
    if (!data.empty()) {
        errors_.fetch_add(1);
        publishError("Remote Agent does not support raw writes; use subscribe controls");
    }
    return false;
}

std::string RemoteAgentSource::sourceId() const {
    return agentSourceId();
}

lab::core::SourceStatistics RemoteAgentSource::statistics() const noexcept {
    return {receivedBytes_.load(),
            transmittedBytes_.load(),
            receivedChunks_.load(),
            transmittedChunks_.load(),
            errors_.load()};
}

bool RemoteAgentSource::requestTopicCatalog() {
    if (!ioThread_.isRunning()) return false;
    bool result = false;
    QMetaObject::invokeMethod(
        worker_, [this, &result] { result = worker_->requestTopicCatalog(); },
        Qt::BlockingQueuedConnection);
    return result;
}

bool RemoteAgentSource::subscribe(
    const lab::core::agent::SubscriptionRequest& request) {
    if (!ioThread_.isRunning()) return false;
    bool result = false;
    QMetaObject::invokeMethod(
        worker_, [this, request, &result] { result = worker_->subscribe(request); },
        Qt::BlockingQueuedConnection);
    return result;
}

bool RemoteAgentSource::unsubscribe(
    const lab::core::agent::SubscriptionRequest& request) {
    if (!ioThread_.isRunning()) return false;
    bool result = false;
    QMetaObject::invokeMethod(
        worker_, [this, request, &result] { result = worker_->unsubscribe(request); },
        Qt::BlockingQueuedConnection);
    return result;
}

HandshakeState RemoteAgentSource::handshakeState() const noexcept {
    return handshakeState_.load();
}

std::optional<lab::core::ClockSyncEstimate>
RemoteAgentSource::clockSyncEstimate() const {
    std::scoped_lock lock(clockSyncMutex_);
    return clockSyncEstimate_;
}

void RemoteAgentSource::handleHello(const lab::core::agent::Hello& hello) {
    {
        std::scoped_lock lock(agentIdentityMutex_);
        agentId_ = hello.agentId;
    }
    const auto callbacks = agentCallbacks();
    if (callbacks.onHello) callbacks.onHello(hello);
}

void RemoteAgentSource::handleSample(
    const lab::core::agent::Frame& frame,
    const lab::core::agent::SampleBatch& batch) {
    const auto timestamp = frame.sourceTimestamp != 0
                               ? frame.sourceTimestamp
                               : (frame.agentReceiveTimestamp != 0
                                      ? frame.agentReceiveTimestamp
                                      : lab::core::nowTimestampNs());
    const auto id = agentSourceId(batch.topic);
    if (!batch.serializedData.empty()) {
        publishData({id,
                     frame.sourceTimestamp,
                     lab::core::nowTimestampNs(),
                     frame.sequence,
                     lab::core::Direction::Rx,
                     batch.serializedData});
    }
    for (const auto& field : batch.fields) {
        double value = 0.0;
        if (const auto* numeric = std::get_if<double>(&field.value)) {
            value = *numeric;
        } else if (const auto* boolean = std::get_if<bool>(&field.value)) {
            value = *boolean ? 1.0 : 0.0;
        } else {
            continue;
        }
        publishSample({timestamp,
                       id,
                       batch.topic + "." + field.path,
                       value,
                       field.unit,
                       frame.sequence});
    }
    const auto callbacks = agentCallbacks();
    if (callbacks.onSampleBatch) callbacks.onSampleBatch(batch);
}

void RemoteAgentSource::handleProtocolIssue(
    const lab::core::agent::DecodeIssue& issue) {
    const auto callbacks = agentCallbacks();
    if (callbacks.onProtocolIssue) callbacks.onProtocolIssue(issue);
}

void RemoteAgentSource::handleClockSync(
    const lab::core::ClockSyncEstimate& estimate) {
    {
        std::scoped_lock lock(clockSyncMutex_);
        clockSyncEstimate_ = estimate;
    }
    const auto callbacks = agentCallbacks();
    if (callbacks.onClockSync) callbacks.onClockSync(estimate);
}

void RemoteAgentSource::handleWireBytes(
    lab::core::Direction direction,
    std::size_t count) {
    if (direction == lab::core::Direction::Rx) {
        receivedBytes_.fetch_add(count);
        receivedChunks_.fetch_add(1);
    } else {
        transmittedBytes_.fetch_add(count);
        transmittedChunks_.fetch_add(1);
    }
}

RemoteAgentCallbacks RemoteAgentSource::agentCallbacks() const {
    std::scoped_lock lock(agentCallbackMutex_);
    return agentCallbacks_;
}

std::string RemoteAgentSource::agentSourceId(const std::string& topic) const {
    std::string id;
    {
        std::scoped_lock lock(agentIdentityMutex_);
        id = agentId_;
    }
    if (id.empty()) {
        const auto configuration = settings();
        id = configuration.host + ':' + std::to_string(configuration.port);
    }
    auto result = "ros-agent:" + id;
    if (!topic.empty()) {
        result += ':' + topic;
    }
    return result;
}

std::string toString(HandshakeState state) {
    switch (state) {
    case HandshakeState::Disconnected: return "disconnected";
    case HandshakeState::AwaitingHello: return "awaiting_hello";
    case HandshakeState::Ready: return "ready";
    case HandshakeState::Error: return "error";
    }
    return "unknown";
}

}  // namespace lab::adapters::remote_agent

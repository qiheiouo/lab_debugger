#include "lab_debug_agent/tcp_server.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <exception>
#include <utility>
#include <variant>

namespace lab_debug_agent {
namespace {

void closeSocket(int descriptor) {
    if (descriptor < 0) return;
    ::shutdown(descriptor, SHUT_RDWR);
    ::close(descriptor);
}

std::string socketError(const std::string& operation) {
    return operation + ": " + std::strerror(errno);
}

}  // namespace

AgentTcpServer::AgentTcpServer(
    TcpServerSettings settings,
    lab::core::agent::Hello identity,
    TcpServerCallbacks callbacks)
    : settings_(std::move(settings)),
      session_(std::move(identity)),
      callbacks_(std::move(callbacks)) {}

AgentTcpServer::~AgentTcpServer() {
    stop();
}

bool AgentTcpServer::start(std::string* error) {
    if (thread_.joinable()) {
        if (error) *error = "Agent TCP server is already running";
        return false;
    }
    if (settings_.port == 0) {
        if (error) *error = "Agent TCP port must not be zero";
        return false;
    }

    const auto descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) {
        if (error) *error = socketError("socket");
        return false;
    }
    int reuse = 1;
    ::setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(settings_.port);
    if (::inet_pton(AF_INET, settings_.bindAddress.c_str(), &address.sin_addr) != 1) {
        closeSocket(descriptor);
        if (error) *error = "Agent bind address must be a numeric IPv4 address";
        return false;
    }
    if (::bind(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        if (error) *error = socketError("bind");
        closeSocket(descriptor);
        return false;
    }
    if (::listen(descriptor, 1) < 0) {
        if (error) *error = socketError("listen");
        closeSocket(descriptor);
        return false;
    }
    listener_.store(descriptor);
    thread_ = std::jthread([this](std::stop_token token) { run(token); });
    info("Listening on " + settings_.bindAddress + ':' + std::to_string(settings_.port));
    return true;
}

void AgentTcpServer::stop() {
    if (thread_.joinable()) thread_.request_stop();
    closeListener();
    closeClient();
    if (thread_.joinable()) thread_.join();
    {
        std::scoped_lock lock(sessionIoMutex_);
        session_.reset();
    }
}

bool AgentTcpServer::publishCatalog(
    const lab::core::agent::TopicCatalog& catalog) {
    std::scoped_lock lock(sessionIoMutex_);
    try {
        const auto bytes = session_.makeTopicCatalog(catalog);
        return bytes && sendEncoded(*bytes);
    } catch (const std::exception& exception) {
        warning(std::string("Failed to encode topic catalog: ") + exception.what());
        return false;
    }
}

bool AgentTcpServer::publishTopicFieldCatalog(
    const lab::core::agent::TopicFieldCatalog& catalog) {
    std::scoped_lock lock(sessionIoMutex_);
    try {
        const auto bytes = session_.makeTopicFieldCatalog(catalog);
        return bytes && sendEncoded(*bytes);
    } catch (const std::exception& exception) {
        warning(std::string("Failed to encode topic field catalog: ") +
                exception.what());
        return false;
    }
}

bool AgentTcpServer::publishSample(
    const lab::core::agent::SampleBatch& sample,
    lab::core::Timestamp sourceTimestamp,
    lab::core::Timestamp agentReceiveTimestamp) {
    std::scoped_lock lock(sessionIoMutex_);
    try {
        const auto bytes = session_.makeSample(
            sample, sourceTimestamp, agentReceiveTimestamp);
        return bytes && sendEncoded(*bytes);
    } catch (const std::exception& exception) {
        warning(std::string("Failed to encode sample: ") + exception.what());
        return false;
    }
}

bool AgentTcpServer::publishError(const lab::core::agent::AgentError& error) {
    std::scoped_lock lock(sessionIoMutex_);
    try {
        const auto bytes = session_.makeError(error);
        return bytes && sendEncoded(*bytes);
    } catch (const std::exception& exception) {
        warning(std::string("Failed to encode Agent error: ") + exception.what());
        return false;
    }
}

bool AgentTcpServer::publishPing(std::uint64_t nonce) {
    std::scoped_lock lock(sessionIoMutex_);
    try {
        const auto bytes = session_.makePing(nonce);
        return bytes && sendEncoded(*bytes);
    } catch (const std::exception& exception) {
        warning(std::string("Failed to encode heartbeat: ") + exception.what());
        return false;
    }
}

bool AgentTcpServer::clientReady() const noexcept {
    return client_.load() >= 0 &&
           session_.state() == lab::core::agent::ServerSessionState::Ready;
}

std::uint32_t AgentTcpServer::negotiatedCapabilities() const noexcept {
    return session_.negotiatedCapabilities();
}

void AgentTcpServer::run(std::stop_token stopToken) {
    while (!stopToken.stop_requested()) {
        sockaddr_in peerAddress{};
        socklen_t peerLength = sizeof(peerAddress);
        const auto accepted = ::accept(
            listener_.load(), reinterpret_cast<sockaddr*>(&peerAddress), &peerLength);
        if (accepted < 0) {
            if (!stopToken.stop_requested()) warning(socketError("accept"));
            break;
        }
        client_.store(accepted);
        timeval sendTimeout{};
        sendTimeout.tv_sec = 2;
        ::setsockopt(
            accepted, SOL_SOCKET, SO_SNDTIMEO, &sendTimeout, sizeof(sendTimeout));
        char peerText[INET_ADDRSTRLEN]{};
        ::inet_ntop(AF_INET, &peerAddress.sin_addr, peerText, sizeof(peerText));
        info(std::string("Client connected: ") + peerText + ':' +
             std::to_string(ntohs(peerAddress.sin_port)));
        serveClient(accepted, stopToken);
        closeClient();
        {
            std::scoped_lock lock(sessionIoMutex_);
            session_.reset();
        }
        if (callbacks_.onClientDisconnected) callbacks_.onClientDisconnected();
        info("Client disconnected");
    }
}

void AgentTcpServer::serveClient(int client, std::stop_token stopToken) {
    {
        std::scoped_lock lock(sessionIoMutex_);
        if (!sendEncoded(session_.start())) return;
    }
    std::array<std::uint8_t, 64U * 1024U> buffer{};
    while (!stopToken.stop_requested() && client_.load() == client) {
        const auto received = ::recv(client, buffer.data(), buffer.size(), 0);
        if (received == 0) return;
        if (received < 0) {
            if (errno == EINTR) continue;
            warning(socketError("recv"));
            return;
        }
        lab::core::agent::ServerConsumeResult result;
        {
            std::scoped_lock lock(sessionIoMutex_);
            result = session_.consume(
                std::span(buffer.data(), static_cast<std::size_t>(received)));
            for (const auto& bytes : result.outboundFrames) {
                if (!sendEncoded(bytes)) return;
            }
        }
        for (const auto& issue : result.decodeIssues) {
            warning("Protocol stream issue: " + issue.message);
        }
        for (const auto& action : result.actions) processAction(action);
        if (result.fatalError) {
            warning("Closing protocol session: " + *result.fatalError);
            return;
        }
    }
}

bool AgentTcpServer::sendEncoded(const std::vector<std::uint8_t>& bytes) {
    std::scoped_lock lock(sendMutex_);
    const auto descriptor = client_.load();
    if (descriptor < 0) return false;
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const auto result = ::send(
            descriptor,
            bytes.data() + sent,
            bytes.size() - sent,
            MSG_NOSIGNAL);
        if (result < 0) {
            if (errno == EINTR) continue;
            warning(socketError("send"));
            return false;
        }
        if (result == 0) return false;
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

void AgentTcpServer::closeListener() {
    closeSocket(listener_.exchange(-1));
}

void AgentTcpServer::closeClient() {
    std::scoped_lock lock(sendMutex_);
    closeSocket(client_.exchange(-1));
}

void AgentTcpServer::processAction(
    const lab::core::agent::ServerAction& action) {
    try {
        if (std::holds_alternative<lab::core::agent::CatalogRequestAction>(action)) {
            if (callbacks_.onCatalogRequested) {
                const auto catalog = callbacks_.onCatalogRequested();
                const auto catalogSent = publishCatalog(catalog);
                if (catalogSent &&
                    (negotiatedCapabilities() &
                     lab::core::agent::capabilityMask(
                         lab::core::agent::Capability::TopicFieldCapabilities)) != 0U &&
                    callbacks_.onTopicFieldCatalogRequested) {
                    publishTopicFieldCatalog(
                        callbacks_.onTopicFieldCatalogRequested(catalog));
                }
            }
            return;
        }
        if (const auto* subscribe =
                std::get_if<lab::core::agent::SubscribeAction>(&action)) {
            const auto failure = callbacks_.onSubscribe
                                     ? callbacks_.onSubscribe(subscribe->request)
                                     : std::optional<std::string>("Subscribe handler unavailable");
            if (failure) {
                publishError({2001U, subscribe->request.topic, *failure});
            }
            return;
        }
        if (const auto* unsubscribe =
                std::get_if<lab::core::agent::UnsubscribeAction>(&action)) {
            const auto failure = callbacks_.onUnsubscribe
                                     ? callbacks_.onUnsubscribe(unsubscribe->request)
                                     : std::optional<std::string>("Unsubscribe handler unavailable");
            if (failure) {
                publishError({2002U, unsubscribe->request.topic, *failure});
            }
            return;
        }
        if (const auto* peerError =
                std::get_if<lab::core::agent::PeerErrorAction>(&action)) {
            warning("Client error " + std::to_string(peerError->error.code) + " [" +
                    peerError->error.context + "]: " + peerError->error.message);
        }
    } catch (const std::exception& exception) {
        warning(std::string("Agent action handler failed: ") + exception.what());
        publishError({2000U, "action", exception.what()});
    }
}

void AgentTcpServer::info(const std::string& message) const {
    if (callbacks_.onInfo) callbacks_.onInfo(message);
}

void AgentTcpServer::warning(const std::string& message) const {
    if (callbacks_.onWarning) callbacks_.onWarning(message);
}

}  // namespace lab_debug_agent

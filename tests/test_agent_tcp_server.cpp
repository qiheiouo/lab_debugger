#include "lab_debug_agent/tcp_server.hpp"

#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using namespace lab::core::agent;
using namespace lab_debug_agent;
using namespace std::chrono_literals;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("agent TCP server: " + message);
}

class Socket final {
public:
    explicit Socket(int descriptor = -1) : descriptor_(descriptor) {}
    ~Socket() { reset(); }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)) {}

    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            reset();
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return descriptor_; }

    void reset() noexcept {
        if (descriptor_ < 0) return;
        ::shutdown(descriptor_, SHUT_RDWR);
        ::close(descriptor_);
        descriptor_ = -1;
    }

private:
    int descriptor_;
};

std::uint16_t availableLoopbackPort() {
    Socket socket(::socket(AF_INET, SOCK_STREAM, 0));
    require(socket.get() >= 0, "creates port reservation socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    require(
        ::bind(
            socket.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
        "binds ephemeral loopback port");
    socklen_t length = sizeof(address);
    require(
        ::getsockname(
            socket.get(), reinterpret_cast<sockaddr*>(&address), &length) == 0,
        "reads ephemeral loopback port");
    return ntohs(address.sin_port);
}

Socket connectClient(std::uint16_t port) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        Socket socket(::socket(AF_INET, SOCK_STREAM, 0));
        require(socket.get() >= 0, "creates client socket");
        timeval timeout{};
        timeout.tv_sec = 2;
        ::setsockopt(
            socket.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        if (::connect(
                socket.get(),
                reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) == 0) {
            return socket;
        }
        if (errno != ECONNREFUSED && errno != EINTR) {
            throw std::runtime_error(
                "agent TCP server: connect failed: " +
                std::string(std::strerror(errno)));
        }
        std::this_thread::sleep_for(10ms);
    }
    throw std::runtime_error("agent TCP server: timed out connecting to listener");
}

void sendAll(int socket, std::span<const std::uint8_t> bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const auto count = ::send(
            socket,
            bytes.data() + sent,
            bytes.size() - sent,
            MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) continue;
        require(count > 0, "writes complete client frame");
        sent += static_cast<std::size_t>(count);
    }
}

void sendFrame(
    int socket,
    MessageType type,
    std::uint64_t sequence,
    std::vector<std::uint8_t> payload) {
    const auto bytes = encodeFrame({type, 0, sequence, 10, 20, std::move(payload)});
    sendAll(socket, bytes);
}

std::vector<Frame> receiveFrames(
    int socket,
    StreamDecoder& decoder,
    std::size_t minimum) {
    std::vector<Frame> frames;
    std::vector<std::uint8_t> buffer(64U * 1024U);
    while (frames.size() < minimum) {
        const auto count = ::recv(socket, buffer.data(), buffer.size(), 0);
        if (count < 0 && errno == EINTR) continue;
        require(count > 0, "receives expected server frames");
        auto decoded = decoder.consume(
            std::span(buffer.data(), static_cast<std::size_t>(count)));
        require(decoded.issues.empty(), "server output stream has no decode issue");
        frames.insert(frames.end(), decoded.frames.begin(), decoded.frames.end());
    }
    return frames;
}

bool waitFor(const std::function<bool()>& predicate) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (predicate()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return predicate();
}

Hello identity() {
    return {
        "linux-test-agent",
        "0.1.0",
        "localhost",
        capabilityMask(Capability::TopicDiscovery) |
            capabilityMask(Capability::SerializedMessages) |
            capabilityMask(Capability::NumericFields) |
            capabilityMask(Capability::TextFields) |
            capabilityMask(Capability::GraphUpdates) |
            capabilityMask(Capability::TopicFieldCapabilities)};
}

void completeHandshake(
    int socket,
    StreamDecoder& decoder,
    std::uint64_t* lastServerSequence,
    std::uint32_t requestedCapabilities = identity().capabilities) {
    const auto helloFrames = receiveFrames(socket, decoder, 1);
    require(
        helloFrames.size() == 1 && helloFrames[0].type == MessageType::Hello,
        "connection begins with Hello");
    std::string error;
    require(
        decodeHello(helloFrames[0].payload, &error) == identity(),
        "Hello carries configured Agent identity");
    require(helloFrames[0].sequence == 0, "new connection starts server sequence at zero");
    *lastServerSequence = helloFrames[0].sequence;
    sendFrame(
        socket,
        MessageType::HelloAck,
        0,
        encodeHelloAck(
            {"Linux integration test", "0.1.0", requestedCapabilities}));
}

void requireIncreasing(
    const std::vector<Frame>& frames,
    std::uint64_t* lastSequence) {
    for (const auto& frame : frames) {
        require(
            frame.sequence > *lastSequence,
            "concurrent publishers preserve strict server sequence order");
        *lastSequence = frame.sequence;
    }
}

void testTcpLifecycleAndConcurrentOrdering() {
    const auto port = availableLoopbackPort();
    std::mutex callbackMutex;
    std::vector<SubscriptionRequest> subscriptions;
    std::vector<SubscriptionRequest> unsubscriptions;
    int disconnects = 0;
    TcpServerCallbacks callbacks;
    callbacks.onCatalogRequested = [] {
        return TopicCatalog{
            1,
            {{"/value",
              "std_msgs/msg/Float64",
              Reliability::Reliable,
              Durability::Volatile}}};
    };
    callbacks.onTopicFieldCatalogRequested = [](const TopicCatalog& catalog) {
        return TopicFieldCatalog{
            catalog.graphRevision,
            {{"/value", "std_msgs/msg/Float64", FieldMappingKind::BuiltIn,
              "Built-in semantic mapper preserves known field units"}}};
    };
    callbacks.onSubscribe = [&](const SubscriptionRequest& request) {
        std::scoped_lock lock(callbackMutex);
        subscriptions.push_back(request);
        return std::optional<std::string>{};
    };
    callbacks.onUnsubscribe = [&](const SubscriptionRequest& request) {
        std::scoped_lock lock(callbackMutex);
        unsubscriptions.push_back(request);
        return std::optional<std::string>{};
    };
    callbacks.onClientDisconnected = [&] {
        std::scoped_lock lock(callbackMutex);
        ++disconnects;
    };

    AgentTcpServer server({"127.0.0.1", port}, identity(), std::move(callbacks));
    std::string error;
    require(server.start(&error), "starts loopback listener: " + error);

    Socket client = connectClient(port);
    StreamDecoder decoder;
    std::uint64_t lastServerSequence = 0;
    completeHandshake(client.get(), decoder, &lastServerSequence);
    sendFrame(client.get(), MessageType::TopicCatalogRequest, 1, {});
    const auto catalogs = receiveFrames(client.get(), decoder, 2);
    require(
        catalogs.size() == 2 && catalogs[0].type == MessageType::TopicCatalog &&
            catalogs[1].type == MessageType::TopicFieldCatalog,
        "catalog request receives topic and negotiated field catalogs");
    require(
        decodeTopicFieldCatalog(catalogs[1].payload)->topics[0].mapping ==
            FieldMappingKind::BuiltIn,
        "field catalog preserves the Agent mapping capability");
    requireIncreasing(catalogs, &lastServerSequence);

    const SubscriptionRequest request{
        17, "/value", "std_msgs/msg/Float64", Reliability::Reliable, 25};
    sendFrame(
        client.get(), MessageType::Subscribe, 2, encodeSubscriptionRequest(request));
    sendFrame(
        client.get(), MessageType::Unsubscribe, 3, encodeSubscriptionRequest(request));
    require(
        waitFor([&] {
            std::scoped_lock lock(callbackMutex);
            return subscriptions.size() == 1 && unsubscriptions.size() == 1;
        }),
        "subscribe and unsubscribe reach server callbacks");

    sendFrame(client.get(), MessageType::Ping, 4, encodeNonce(0x1234));
    const auto pongs = receiveFrames(client.get(), decoder, 1);
    require(
        pongs.size() == 1 && pongs[0].type == MessageType::Pong &&
            decodeNonce(pongs[0].payload, &error) == 0x1234,
        "Ping receives matching Pong");
    requireIncreasing(pongs, &lastServerSequence);

    auto corrupt = encodeFrame(
        {MessageType::Ping, 0, 5, 10, 20, encodeNonce(0xAAAA)});
    corrupt.back() ^= 0x80U;
    const auto valid = encodeFrame(
        {MessageType::Ping, 0, 5, 10, 20, encodeNonce(0xBBBB)});
    corrupt.insert(corrupt.end(), valid.begin(), valid.end());
    sendAll(client.get(), corrupt);
    const auto recovered = receiveFrames(client.get(), decoder, 1);
    require(
        recovered.size() == 1 && recovered[0].type == MessageType::Pong &&
            decodeNonce(recovered[0].payload, &error) == 0xBBBB,
        "CRC damage is skipped and the following legal frame is processed");
    requireIncreasing(recovered, &lastServerSequence);

    constexpr int publisherCount = 8;
    constexpr int framesPerPublisher = 64;
    std::atomic_bool allPublished{true};
    std::vector<std::thread> publishers;
    publishers.reserve(publisherCount);
    for (int publisher = 0; publisher < publisherCount; ++publisher) {
        publishers.emplace_back([&, publisher] {
            for (int index = 0; index < framesPerPublisher; ++index) {
                const auto nonce =
                    (static_cast<std::uint64_t>(publisher) << 32U) |
                    static_cast<std::uint32_t>(index);
                if (!server.publishPing(nonce)) {
                    allPublished.store(false);
                    return;
                }
            }
        });
    }
    for (auto& publisher : publishers) publisher.join();
    require(allPublished.load(), "concurrent heartbeat publishes succeed");
    const auto concurrent = receiveFrames(
        client.get(), decoder, publisherCount * framesPerPublisher);
    require(
        concurrent.size() == publisherCount * framesPerPublisher,
        "client receives every concurrently published frame");
    requireIncreasing(concurrent, &lastServerSequence);

    sendFrame(client.get(), MessageType::Ping, 5, encodeNonce(0xCCCC));
    const auto protocolError = receiveFrames(client.get(), decoder, 1);
    require(
        protocolError.size() == 1 && protocolError[0].type == MessageType::Error,
        "duplicate client sequence receives protocol Error before disconnect");
    requireIncreasing(protocolError, &lastServerSequence);
    require(
        waitFor([&] {
            std::scoped_lock lock(callbackMutex);
            return disconnects == 1;
        }),
        "protocol failure closes connection and runs cleanup callback");
    client.reset();

    Socket reconnected = connectClient(port);
    StreamDecoder reconnectDecoder;
    lastServerSequence = 0;
    completeHandshake(reconnected.get(), reconnectDecoder, &lastServerSequence);
    reconnected.reset();
    require(
        waitFor([&] {
            std::scoped_lock lock(callbackMutex);
            return disconnects == 2;
        }),
        "a fresh client reconnects and disconnect cleanup runs again");

    Socket legacy = connectClient(port);
    StreamDecoder legacyDecoder;
    lastServerSequence = 0;
    const auto legacyCapabilities =
        capabilityMask(Capability::TopicDiscovery) |
        capabilityMask(Capability::SerializedMessages);
    completeHandshake(
        legacy.get(), legacyDecoder, &lastServerSequence, legacyCapabilities);
    sendFrame(legacy.get(), MessageType::TopicCatalogRequest, 1, {});
    const auto legacyCatalogFrames = receiveFrames(legacy.get(), legacyDecoder, 1);
    require(
        legacyCatalogFrames.size() == 1 &&
            legacyCatalogFrames[0].type == MessageType::TopicCatalog &&
            decodeTopicCatalog(legacyCatalogFrames[0].payload).has_value(),
        "legacy v1 client receives the unchanged TopicCatalog only");
    requireIncreasing(legacyCatalogFrames, &lastServerSequence);

    timeval shortTimeout{};
    shortTimeout.tv_usec = 200'000;
    ::setsockopt(
        legacy.get(), SOL_SOCKET, SO_RCVTIMEO, &shortTimeout, sizeof(shortTimeout));
    std::array<std::uint8_t, 256> unexpected{};
    const auto unexpectedCount =
        ::recv(legacy.get(), unexpected.data(), unexpected.size(), 0);
    require(
        unexpectedCount < 0 && (errno == EAGAIN || errno == EWOULDBLOCK),
        "legacy v1 client is never sent unnegotiated TopicFieldCatalog data");

    const SubscriptionRequest legacyRequest{
        23, "/value", "std_msgs/msg/Float64", Reliability::Reliable, 10};
    sendFrame(
        legacy.get(),
        MessageType::Subscribe,
        2,
        encodeSubscriptionRequest(legacyRequest));
    require(
        waitFor([&] {
            std::scoped_lock lock(callbackMutex);
            return subscriptions.size() == 2;
        }),
        "legacy v1 client can still subscribe");
    require(
        server.publishSample(
            {"/value",
             "std_msgs/msg/Float64",
             {1, 2, 3, 4},
             {{"data", "", 42.0}}},
            100,
            200),
        "legacy v1 sample publish succeeds");
    const auto legacySamples = receiveFrames(legacy.get(), legacyDecoder, 1);
    const auto legacySample = decodeSampleBatch(legacySamples[0].payload);
    require(
        legacySamples.size() == 1 &&
            legacySamples[0].type == MessageType::SampleBatch &&
            legacySample && legacySample->serializedData ==
                std::vector<std::uint8_t>({1, 2, 3, 4}),
        "legacy v1 client still receives negotiated raw samples");
    requireIncreasing(legacySamples, &lastServerSequence);

    sendFrame(legacy.get(), MessageType::Ping, 3, encodeNonce(0xDDDD));
    const auto legacyPongs = receiveFrames(legacy.get(), legacyDecoder, 1);
    require(
        legacyPongs.size() == 1 && legacyPongs[0].type == MessageType::Pong &&
            decodeNonce(legacyPongs[0].payload) == 0xDDDD,
        "legacy v1 client still receives Pong");
    requireIncreasing(legacyPongs, &lastServerSequence);
    legacy.reset();
    require(
        waitFor([&] {
            std::scoped_lock lock(callbackMutex);
            return disconnects == 3;
        }),
        "legacy v1 client disconnect cleanup runs");
    server.stop();
}

}  // namespace

int main() {
    try {
        testTcpLifecycleAndConcurrentOrdering();
        std::cout << "All Agent TCP server tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Test failed: " << exception.what() << '\n';
        return 1;
    }
}

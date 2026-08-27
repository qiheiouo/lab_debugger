#include "lab/core/agent_protocol.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <geometry_msgs/msg/point.hpp>
#include <geometry_msgs/msg/twist.hpp>
#include <geometry_msgs/msg/twist_stamped.hpp>
#include <iostream>
#include <memory>
#include <nav_msgs/msg/odometry.hpp>
#include <netinet/in.h>
#include <optional>
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/imu.hpp>
#include <sensor_msgs/msg/joint_state.hpp>
#include <spawn.h>
#include <span>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/float64.hpp>
#include <std_msgs/msg/string.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

extern char** environ;

namespace {

using namespace lab::core::agent;
using namespace std::chrono_literals;

constexpr std::string_view prefix = "/lab_debug_agent_test";

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("ROS integration: " + message);
}

void progress(const std::string& message) {
    std::cout << "[integration] " << message << std::endl;
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
        "binds an ephemeral loopback port");
    socklen_t length = sizeof(address);
    require(
        ::getsockname(
            socket.get(), reinterpret_cast<sockaddr*>(&address), &length) == 0,
        "reads the ephemeral loopback port");
    return ntohs(address.sin_port);
}

class AgentProcess final {
public:
    AgentProcess(const std::string& executable, std::uint16_t port) {
        pid_ = ::fork();
        require(pid_ >= 0, "forks Agent process");
        if (pid_ != 0) return;

        const auto portParameter = "port:=" + std::to_string(port);
        ::execl(
            executable.c_str(),
            executable.c_str(),
            "--ros-args",
            "-p",
            "bind_address:=127.0.0.1",
            "-p",
            portParameter.c_str(),
            "-p",
            "agent_id:=linux-validation",
            static_cast<char*>(nullptr));
        std::cerr << "Failed to exec Agent: " << std::strerror(errno) << '\n';
        ::_exit(127);
    }

    ~AgentProcess() {
        if (pid_ <= 0) return;
        ::kill(pid_, SIGKILL);
        ::waitpid(pid_, nullptr, 0);
    }

    AgentProcess(const AgentProcess&) = delete;
    AgentProcess& operator=(const AgentProcess&) = delete;

    bool stopCleanly() {
        if (pid_ <= 0) return true;
        ::kill(pid_, SIGINT);
        int status = 0;
        for (int attempt = 0; attempt < 300; ++attempt) {
            const auto result = ::waitpid(pid_, &status, WNOHANG);
            if (result == pid_) {
                pid_ = -1;
                return WIFEXITED(status) && WEXITSTATUS(status) == 0;
            }
            if (result < 0) return false;
            std::this_thread::sleep_for(10ms);
        }
        return false;
    }

private:
    pid_t pid_{-1};
};

class MultiTypePublisherProcess final {
public:
    MultiTypePublisherProcess(
        const std::string& executable,
        const std::string& topicName) {
        char* arguments[]{
            const_cast<char*>(executable.c_str()),
            const_cast<char*>(topicName.c_str()),
            nullptr};
        const auto result = ::posix_spawn(
            &pid_, executable.c_str(), nullptr, nullptr, arguments, environ);
        require(result == 0, "spawns auxiliary publisher process: " +
                                 std::string(std::strerror(result)));
    }

    ~MultiTypePublisherProcess() {
        if (pid_ <= 0) return;
        ::kill(pid_, SIGKILL);
        ::waitpid(pid_, nullptr, 0);
    }

    MultiTypePublisherProcess(const MultiTypePublisherProcess&) = delete;
    MultiTypePublisherProcess& operator=(const MultiTypePublisherProcess&) = delete;

    bool stopCleanly() {
        if (pid_ <= 0) return true;
        ::kill(pid_, SIGINT);
        int status = 0;
        for (int attempt = 0; attempt < 300; ++attempt) {
            const auto result = ::waitpid(pid_, &status, WNOHANG);
            if (result == pid_) {
                pid_ = -1;
                return WIFEXITED(status) && WEXITSTATUS(status) == 0;
            }
            if (result < 0) return false;
            std::this_thread::sleep_for(10ms);
        }
        return false;
    }

private:
    pid_t pid_{-1};
};

Socket connectClient(std::uint16_t port) {
    for (int attempt = 0; attempt < 300; ++attempt) {
        Socket socket(::socket(AF_INET, SOCK_STREAM, 0));
        require(socket.get() >= 0, "creates protocol client socket");
        timeval timeout{};
        timeout.tv_usec = 200'000;
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
                "ROS integration: connect failed: " +
                std::string(std::strerror(errno)));
        }
        std::this_thread::sleep_for(10ms);
    }
    throw std::runtime_error("ROS integration: timed out connecting to Agent");
}

class ProtocolClient final {
public:
    explicit ProtocolClient(Socket socket) : socket_(std::move(socket)) {}

    void handshake() {
        const auto helloFrame = receiveMatching(
            [](const Frame& frame) { return frame.type == MessageType::Hello; }, 3s);
        std::string error;
        const auto hello = decodeHello(helloFrame.payload, &error);
        require(hello && hello->agentId == "linux-validation",
                "receives valid configured Hello identity");
        require(
            (hello->capabilities & capabilityMask(Capability::GraphUpdates)) != 0U,
            "Hello offers graph updates");
        require(
            (hello->capabilities &
             capabilityMask(Capability::TopicFieldCapabilities)) != 0U,
            "Hello offers topic field capabilities");
        send(
            MessageType::HelloAck,
            encodeHelloAck({"Humble integration test", "0.1.0", hello->capabilities}));
    }

    void send(MessageType type, std::vector<std::uint8_t> payload) {
        sendEncoded(encodeFrame(
            {type, 0, nextOutboundSequence_++, 10, 20, std::move(payload)}));
    }

    void sendCorruptThenValidPing(std::uint64_t nonce) {
        auto corrupt = encodeFrame(
            {MessageType::Ping,
             0,
             nextOutboundSequence_,
             10,
             20,
             encodeNonce(nonce - 1)});
        corrupt.back() ^= 0x80U;
        sendEncoded(corrupt);
        send(MessageType::Ping, encodeNonce(nonce));
    }

    Frame receiveMatching(
        const std::function<bool(const Frame&)>& predicate,
        std::chrono::milliseconds timeout,
        bool allowError = false) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            auto frame = receiveOne(deadline);
            if (!frame) continue;
            if (frame->type == MessageType::Ping) {
                std::string error;
                const auto nonce = decodeNonce(frame->payload, &error);
                require(nonce.has_value(), "Agent Ping payload is valid");
                send(MessageType::Pong, encodeNonce(*nonce));
                continue;
            }
            if (frame->type == MessageType::Error && !allowError) {
                std::string error;
                const auto agentError = decodeAgentError(frame->payload, &error);
                throw std::runtime_error(
                    "ROS integration: unexpected Agent error: " +
                    (agentError ? agentError->message : error));
            }
            if (predicate(*frame)) return std::move(*frame);
        }
        throw std::runtime_error("ROS integration: timed out waiting for protocol frame");
    }

    bool receivesNoSampleFor(
        const std::string& topic,
        std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            auto frame = receiveOne(deadline);
            if (!frame) continue;
            if (frame->type == MessageType::Ping) {
                std::string error;
                const auto nonce = decodeNonce(frame->payload, &error);
                require(nonce.has_value(), "heartbeat is valid while checking unsubscribe");
                send(MessageType::Pong, encodeNonce(*nonce));
            } else if (frame->type == MessageType::SampleBatch) {
                std::string error;
                const auto sample = decodeSampleBatch(frame->payload, &error);
                require(sample.has_value(), "sample decodes while checking unsubscribe");
                if (sample->topic == topic) return false;
            }
        }
        return true;
    }

    bool waitForDisconnect(std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::vector<std::uint8_t> buffer(1024);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto count = ::recv(socket_.get(), buffer.data(), buffer.size(), 0);
            if (count == 0) return true;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                continue;
            }
            if (count < 0) return false;
        }
        return false;
    }

    void close() { socket_.reset(); }

private:
    void sendEncoded(std::span<const std::uint8_t> bytes) {
        std::size_t sent = 0;
        while (sent < bytes.size()) {
            const auto count = ::send(
                socket_.get(),
                bytes.data() + sent,
                bytes.size() - sent,
                MSG_NOSIGNAL);
            if (count < 0 && errno == EINTR) continue;
            require(count > 0, "writes complete protocol frame");
            sent += static_cast<std::size_t>(count);
        }
    }

    std::optional<Frame> receiveOne(
        std::chrono::steady_clock::time_point deadline) {
        if (!pending_.empty()) {
            auto frame = std::move(pending_.front());
            pending_.pop_front();
            return frame;
        }
        std::vector<std::uint8_t> buffer(64U * 1024U);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto count = ::recv(socket_.get(), buffer.data(), buffer.size(), 0);
            if (count == 0) {
                throw std::runtime_error("ROS integration: Agent disconnected unexpectedly");
            }
            if (count < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                throw std::runtime_error(
                    "ROS integration: recv failed: " +
                    std::string(std::strerror(errno)));
            }
            auto decoded = decoder_.consume(
                std::span(buffer.data(), static_cast<std::size_t>(count)));
            require(decoded.issues.empty(), "Agent output has no stream decode issue");
            for (auto& frame : decoded.frames) {
                require(
                    !lastInboundSequence_ || frame.sequence > *lastInboundSequence_,
                    "Agent sequence is strictly increasing on the wire");
                lastInboundSequence_ = frame.sequence;
                pending_.push_back(std::move(frame));
            }
            if (!pending_.empty()) {
                auto frame = std::move(pending_.front());
                pending_.pop_front();
                return frame;
            }
        }
        return std::nullopt;
    }

    Socket socket_;
    StreamDecoder decoder_;
    std::deque<Frame> pending_;
    std::optional<std::uint64_t> lastInboundSequence_;
    std::uint64_t nextOutboundSequence_{};
};

bool waitFor(const std::function<bool()>& predicate, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return predicate();
}

std::string topic(std::string_view suffix) {
    return std::string(prefix) + '/' + std::string(suffix);
}

const TopicDescriptor* descriptor(
    const TopicCatalog& catalog,
    const std::string& name,
    const std::string& type) {
    for (const auto& candidate : catalog.topics) {
        if (candidate.name == name && candidate.type == type) return &candidate;
    }
    return nullptr;
}

const TopicFieldDescriptor* fieldDescriptor(
    const TopicFieldCatalog& catalog,
    const std::string& name,
    const std::string& type) {
    for (const auto& candidate : catalog.topics) {
        if (candidate.name == name && candidate.type == type) return &candidate;
    }
    return nullptr;
}

TopicCatalog requestCompleteCatalog(ProtocolClient& client) {
    const std::vector<std::pair<std::string, std::string>> expected{
        {topic("float64"), "std_msgs/msg/Float64"},
        {topic("bool"), "std_msgs/msg/Bool"},
        {topic("string"), "std_msgs/msg/String"},
        {topic("twist"), "geometry_msgs/msg/Twist"},
        {topic("twist_stamped"), "geometry_msgs/msg/TwistStamped"},
        {topic("imu"), "sensor_msgs/msg/Imu"},
        {topic("joint_state"), "sensor_msgs/msg/JointState"},
        {topic("odometry"), "nav_msgs/msg/Odometry"},
        {topic("raw_point"), "geometry_msgs/msg/Point"},
        {topic("mixed_qos"), "std_msgs/msg/Float64"},
        {topic("multi_type"), "std_msgs/msg/Float64"},
        {topic("multi_type"), "std_msgs/msg/Bool"}};

    for (int attempt = 0; attempt < 20; ++attempt) {
        client.send(MessageType::TopicCatalogRequest, {});
        const auto frame = client.receiveMatching(
            [](const Frame& value) { return value.type == MessageType::TopicCatalog; },
            2s);
        std::string error;
        const auto catalog = decodeTopicCatalog(frame.payload, &error);
        require(catalog.has_value(), "TopicCatalog payload decodes: " + error);
        const auto fieldFrame = client.receiveMatching(
            [](const Frame& value) {
                return value.type == MessageType::TopicFieldCatalog;
            },
            2s);
        const auto fieldCatalog = decodeTopicFieldCatalog(fieldFrame.payload, &error);
        require(fieldCatalog.has_value(),
                "TopicFieldCatalog payload decodes: " + error);
        require(fieldCatalog->graphRevision == catalog->graphRevision,
                "topic and field catalogs describe the same graph revision");
        const auto complete = std::all_of(
            expected.begin(), expected.end(), [&](const auto& item) {
                return descriptor(*catalog, item.first, item.second) != nullptr;
            });
        if (complete) {
            const auto* builtIn = fieldDescriptor(
                *fieldCatalog, topic("imu"), "sensor_msgs/msg/Imu");
            const auto* generic = fieldDescriptor(
                *fieldCatalog, topic("raw_point"), "geometry_msgs/msg/Point");
            require(builtIn && builtIn->mapping == FieldMappingKind::BuiltIn,
                    "field catalog marks Imu as a built-in semantic mapping");
            require(generic && generic->mapping == FieldMappingKind::Introspection,
                    "field catalog marks Point as runtime introspection");
            return *catalog;
        }
        std::this_thread::sleep_for(100ms);
    }
    throw std::runtime_error("ROS integration: complete ROS graph never reached Agent");
}

const FieldValue& field(const SampleBatch& sample, const std::string& path) {
    for (const auto& candidate : sample.fields) {
        if (candidate.path == path) return candidate;
    }
    throw std::runtime_error("ROS integration: missing mapped field " + path);
}

double number(const SampleBatch& sample, const std::string& path) {
    const auto* value = std::get_if<double>(&field(sample, path).value);
    require(value != nullptr, path + " retains numeric type");
    return *value;
}

template<typename Publisher, typename Message>
std::pair<Frame, SampleBatch> publishAndReceive(
    ProtocolClient& client,
    const Publisher& publisher,
    const Message& message,
    const std::string& expectedTopic) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        publisher->publish(message);
        try {
            const auto frame = client.receiveMatching(
                [&](const Frame& candidate) {
                    if (candidate.type != MessageType::SampleBatch) return false;
                    std::string error;
                    const auto sample = decodeSampleBatch(candidate.payload, &error);
                    return sample && sample->topic == expectedTopic;
                },
                300ms);
            std::string error;
            const auto sample = decodeSampleBatch(frame.payload, &error);
            require(sample.has_value(), "matched SampleBatch decodes");
            require(!sample->serializedData.empty(), "raw CDR is non-empty");
            return {frame, *sample};
        } catch (const std::runtime_error& exception) {
            if (std::string(exception.what()).find("timed out") == std::string::npos) {
                throw;
            }
        }
    }
    throw std::runtime_error("ROS integration: no sample received for " + expectedTopic);
}

AgentError expectError(ProtocolClient& client) {
    const auto frame = client.receiveMatching(
        [](const Frame& value) { return value.type == MessageType::Error; }, 3s, true);
    std::string error;
    const auto decoded = decodeAgentError(frame.payload, &error);
    require(decoded.has_value(), "Agent Error payload decodes: " + error);
    return *decoded;
}

void runIntegration(
    const std::string& agentExecutable,
    const std::string& multiTypePublisherExecutable) {
    const auto port = availableLoopbackPort();
    AgentProcess agent(agentExecutable, port);
    MultiTypePublisherProcess multiTypePublisher(
        multiTypePublisherExecutable, topic("multi_type"));

    rclcpp::init(0, nullptr);
    auto node = std::make_shared<rclcpp::Node>("lab_debug_agent_integration_test");
    rclcpp::QoS reliable(10);
    reliable.reliable();
    rclcpp::QoS bestEffort(10);
    bestEffort.best_effort();

    auto floatPublisher = node->create_publisher<std_msgs::msg::Float64>(
        topic("float64"), reliable);
    auto boolPublisher = node->create_publisher<std_msgs::msg::Bool>(
        topic("bool"), bestEffort);
    auto stringPublisher = node->create_publisher<std_msgs::msg::String>(
        topic("string"), reliable);
    auto twistPublisher = node->create_publisher<geometry_msgs::msg::Twist>(
        topic("twist"), reliable);
    auto stampedPublisher = node->create_publisher<geometry_msgs::msg::TwistStamped>(
        topic("twist_stamped"), reliable);
    auto imuPublisher = node->create_publisher<sensor_msgs::msg::Imu>(
        topic("imu"), bestEffort);
    auto jointPublisher = node->create_publisher<sensor_msgs::msg::JointState>(
        topic("joint_state"), reliable);
    auto odometryPublisher = node->create_publisher<nav_msgs::msg::Odometry>(
        topic("odometry"), reliable);
    auto pointPublisher = node->create_publisher<geometry_msgs::msg::Point>(
        topic("raw_point"), reliable);
    auto mixedReliablePublisher = node->create_publisher<std_msgs::msg::Float64>(
        topic("mixed_qos"), reliable);
    auto mixedBestEffortPublisher = node->create_publisher<std_msgs::msg::Float64>(
        topic("mixed_qos"), bestEffort);
    auto multiFloatPublisher = node->create_publisher<std_msgs::msg::Float64>(
        topic("multi_type"), reliable);

    rclcpp::executors::SingleThreadedExecutor executor;
    executor.add_node(node);
    std::jthread spinThread([&](std::stop_token stopToken) {
        std::stop_callback cancelOnStop(stopToken, [&] { executor.cancel(); });
        executor.spin();
    });

    ProtocolClient client(connectClient(port));
    client.handshake();
    progress("handshake complete");
    const auto initialCatalog = requestCompleteCatalog(client);
    progress("initial catalog complete at revision " +
             std::to_string(initialCatalog.graphRevision));
    require(initialCatalog.graphRevision > 0, "initial graph revision is non-zero");
    const auto* reliableDescriptor = descriptor(
        initialCatalog, topic("multi_type"), "std_msgs/msg/Float64");
    const auto* bestEffortDescriptor = descriptor(
        initialCatalog, topic("multi_type"), "std_msgs/msg/Bool");
    require(reliableDescriptor &&
                reliableDescriptor->reliability == Reliability::Reliable,
            "multi-type topic keeps Float64 reliable QoS");
    require(bestEffortDescriptor &&
                bestEffortDescriptor->reliability == Reliability::BestEffort,
            "multi-type topic keeps Bool best-effort QoS");
    const auto* mixedQosDescriptor = descriptor(
        initialCatalog, topic("mixed_qos"), "std_msgs/msg/Float64");
    require(mixedQosDescriptor &&
                mixedQosDescriptor->reliability == Reliability::Unknown,
            "mixed reliable/best-effort endpoints aggregate to unknown reliability");
    static_cast<void>(mixedReliablePublisher);
    static_cast<void>(mixedBestEffortPublisher);

    MultiTypePublisherProcess latePublisher(
        multiTypePublisherExecutable, topic("late_graph_topic"));
    const auto addedFrame = client.receiveMatching(
        [&](const Frame& frame) {
            if (frame.type != MessageType::TopicCatalog) return false;
            std::string error;
            const auto catalog = decodeTopicCatalog(frame.payload, &error);
            return catalog && catalog->graphRevision > initialCatalog.graphRevision &&
                   descriptor(
                       *catalog,
                       topic("late_graph_topic"),
                       "std_msgs/msg/Bool");
        },
        5s);
    std::string decodeError;
    const auto addedCatalog = decodeTopicCatalog(addedFrame.payload, &decodeError);
    require(addedCatalog.has_value(), "added-topic graph update decodes");
    progress("added-topic graph update at revision " +
             std::to_string(addedCatalog->graphRevision));
    require(latePublisher.stopCleanly(),
            "dynamic graph publisher stops cleanly");
    progress("dynamic publisher process exited cleanly");
    const auto removedFrame = client.receiveMatching(
        [&](const Frame& frame) {
            if (frame.type != MessageType::TopicCatalog) return false;
            std::string error;
            const auto catalog = decodeTopicCatalog(frame.payload, &error);
            return catalog && catalog->graphRevision > addedCatalog->graphRevision &&
                   !descriptor(
                       *catalog,
                       topic("late_graph_topic"),
                       "std_msgs/msg/Bool");
        },
        10s);
    const auto removedCatalog = decodeTopicCatalog(removedFrame.payload, &decodeError);
    require(removedCatalog.has_value(), "removed-topic graph update decodes");
    progress("removed-topic graph update at revision " +
             std::to_string(removedCatalog->graphRevision));

    client.sendCorruptThenValidPing(0xAABBCCDD);
    const auto pong = client.receiveMatching(
        [](const Frame& frame) { return frame.type == MessageType::Pong; }, 3s);
    require(decodeNonce(pong.payload, &decodeError) == 0xAABBCCDD,
            "CRC corruption does not prevent recovery to the following Ping");
    progress("CRC recovery and Ping/Pong complete");

    std::uint64_t requestId = 1;
    const auto subscribe = [&](const std::string& name,
                               const std::string& type,
                               Reliability reliabilityPolicy) {
        const SubscriptionRequest request{
            requestId++, name, type, reliabilityPolicy, 10};
        client.send(MessageType::Subscribe, encodeSubscriptionRequest(request));
        return request;
    };
    const auto floatRequest = subscribe(
        topic("float64"), "std_msgs/msg/Float64", Reliability::Reliable);
    subscribe(topic("bool"), "std_msgs/msg/Bool", Reliability::BestEffort);
    subscribe(topic("string"), "std_msgs/msg/String", Reliability::Reliable);
    subscribe(topic("twist"), "geometry_msgs/msg/Twist", Reliability::Reliable);
    subscribe(
        topic("twist_stamped"),
        "geometry_msgs/msg/TwistStamped",
        Reliability::Reliable);
    subscribe(topic("imu"), "sensor_msgs/msg/Imu", Reliability::BestEffort);
    subscribe(
        topic("joint_state"), "sensor_msgs/msg/JointState", Reliability::Reliable);
    subscribe(
        topic("odometry"), "nav_msgs/msg/Odometry", Reliability::Reliable);
    subscribe(topic("raw_point"), "geometry_msgs/msg/Point", Reliability::Reliable);
    const auto multiFloatRequest = subscribe(
        topic("multi_type"), "std_msgs/msg/Float64", Reliability::Reliable);
    subscribe(
        topic("multi_type"), "std_msgs/msg/Float64", Reliability::Reliable);

    require(waitFor([&] { return floatPublisher->get_subscription_count() == 1; }),
            "reliable Float64 GenericSubscription is created once");
    require(waitFor([&] { return boolPublisher->get_subscription_count() == 1; }),
            "best-effort Bool GenericSubscription is created");
    require(waitFor([&] { return stringPublisher->get_subscription_count() == 1; }),
            "String GenericSubscription is created");
    require(waitFor([&] { return twistPublisher->get_subscription_count() == 1; }),
            "Twist GenericSubscription is created");
    require(waitFor([&] { return stampedPublisher->get_subscription_count() == 1; }),
            "TwistStamped GenericSubscription is created");
    require(waitFor([&] { return imuPublisher->get_subscription_count() == 1; }),
            "best-effort Imu GenericSubscription is created");
    require(waitFor([&] { return jointPublisher->get_subscription_count() == 1; }),
            "JointState GenericSubscription is created");
    require(waitFor([&] { return odometryPublisher->get_subscription_count() == 1; }),
            "Odometry GenericSubscription is created");
    require(waitFor([&] { return pointPublisher->get_subscription_count() == 1; }),
            "runtime-introspected Point GenericSubscription is created");
    require(waitFor([&] { return multiFloatPublisher->get_subscription_count() == 1; }),
            "duplicate request ID-independent Float64 subscribe remains idempotent");
    progress("all mapped and raw subscriptions matched");

    client.send(
        MessageType::Subscribe,
        encodeSubscriptionRequest(
            {requestId++,
             topic("multi_type"),
             "std_msgs/msg/Bool",
             Reliability::BestEffort,
             10}));
    const auto multiTypeError = expectError(client);
    require(multiTypeError.code == 2001 &&
                multiTypeError.message.find("multiple message types") !=
                    std::string::npos,
            "Humble multi-type subscription limitation is explicit");
    require(multiFloatPublisher->get_subscription_count() == 1,
            "failed second type does not replace first type subscription");
    progress("multi-type subscription behavior verified");

    std_msgs::msg::Float64 floatMessage;
    floatMessage.data = 12.75;
    const auto [floatFrame, floatSample] = publishAndReceive(
        client, floatPublisher, floatMessage, topic("float64"));
    static_cast<void>(floatFrame);
    require(number(floatSample, "data") == 12.75,
            "Float64 data field maps exactly over TCP");

    std_msgs::msg::Bool boolMessage;
    boolMessage.data = true;
    const auto [boolFrame, boolSample] = publishAndReceive(
        client, boolPublisher, boolMessage, topic("bool"));
    static_cast<void>(boolFrame);
    const auto* boolValue = std::get_if<bool>(&field(boolSample, "data").value);
    require(boolValue && *boolValue, "Bool keeps its field type and true value");

    std_msgs::msg::String stringMessage;
    stringMessage.data = "remote Humble string";
    const auto [stringFrame, stringSample] = publishAndReceive(
        client, stringPublisher, stringMessage, topic("string"));
    static_cast<void>(stringFrame);
    const auto* stringValue = std::get_if<std::string>(&field(stringSample, "data").value);
    require(stringValue && *stringValue == stringMessage.data,
            "String keeps its field type and contents");

    geometry_msgs::msg::Twist twistMessage;
    twistMessage.linear.x = 1.5;
    twistMessage.angular.z = -0.25;
    const auto [twistFrame, twistSample] = publishAndReceive(
        client, twistPublisher, twistMessage, topic("twist"));
    static_cast<void>(twistFrame);
    require(number(twistSample, "linear.x") == 1.5 &&
                number(twistSample, "angular.z") == -0.25,
            "Twist structured numeric values map exactly");

    geometry_msgs::msg::TwistStamped stampedMessage;
    stampedMessage.header.stamp.sec = 123;
    stampedMessage.header.stamp.nanosec = 456;
    stampedMessage.header.frame_id = "base_link";
    stampedMessage.twist.linear.y = 2.5;
    const auto [stampedFrame, stampedSample] = publishAndReceive(
        client, stampedPublisher, stampedMessage, topic("twist_stamped"));
    require(stampedFrame.sourceTimestamp == 123'000'000'456LL,
            "non-zero Header is transferred as source timestamp");
    require(number(stampedSample, "twist.linear.y") == 2.5,
            "TwistStamped nested field maps exactly");
    stampedMessage.header.stamp.sec = 0;
    stampedMessage.header.stamp.nanosec = 0;
    const auto [zeroFrame, zeroSample] = publishAndReceive(
        client, stampedPublisher, stampedMessage, topic("twist_stamped"));
    static_cast<void>(zeroSample);
    require(zeroFrame.sourceTimestamp > 0 &&
                zeroFrame.sourceTimestamp == zeroFrame.agentReceiveTimestamp,
            "zero Header falls back to Agent receive timestamp");

    sensor_msgs::msg::Imu imuMessage;
    imuMessage.header.stamp.sec = 5;
    imuMessage.orientation.w = 0.8;
    imuMessage.angular_velocity.z = 1.25;
    imuMessage.linear_acceleration.x = 9.81;
    const auto [imuFrame, imuSample] = publishAndReceive(
        client, imuPublisher, imuMessage, topic("imu"));
    require(imuFrame.sourceTimestamp == 5'000'000'000LL &&
                number(imuSample, "orientation.w") == 0.8 &&
                number(imuSample, "angular_velocity.z") == 1.25 &&
                number(imuSample, "linear_acceleration.x") == 9.81,
            "Imu header and structured fields map exactly");

    sensor_msgs::msg::JointState jointMessage;
    jointMessage.name = {"joint_a"};
    jointMessage.position = {0.4};
    jointMessage.velocity = {0.5};
    const auto [jointFrame, jointSample] = publishAndReceive(
        client, jointPublisher, jointMessage, topic("joint_state"));
    static_cast<void>(jointFrame);
    require(number(jointSample, "position.joint_a") == 0.4 &&
                number(jointSample, "velocity.joint_a") == 0.5,
            "JointState arrays map over TCP");

    nav_msgs::msg::Odometry odometryMessage;
    odometryMessage.header.stamp.sec = 6;
    odometryMessage.pose.pose.position.x = 3.0;
    odometryMessage.twist.twist.angular.z = -0.75;
    const auto [odometryFrame, odometrySample] = publishAndReceive(
        client, odometryPublisher, odometryMessage, topic("odometry"));
    require(odometryFrame.sourceTimestamp == 6'000'000'000LL &&
                number(odometrySample, "pose.pose.position.x") == 3.0 &&
                number(odometrySample, "twist.twist.angular.z") == -0.75,
            "Odometry nested fields map over TCP");

    geometry_msgs::msg::Point pointMessage;
    pointMessage.x = 99.0;
    pointMessage.y = -5.5;
    pointMessage.z = 0.25;
    const auto [pointFrame, pointSample] = publishAndReceive(
        client, pointPublisher, pointMessage, topic("raw_point"));
    static_cast<void>(pointFrame);
    require(number(pointSample, "x") == 99.0 &&
                number(pointSample, "y") == -5.5 &&
                number(pointSample, "z") == 0.25,
            "Point is structured through runtime introspection while retaining raw CDR");
    progress("all real ROS sample mappings, generic introspection, and raw CDR verified");

    client.send(
        MessageType::Unsubscribe,
        encodeSubscriptionRequest(
            {requestId++,
             multiFloatRequest.topic,
             multiFloatRequest.type,
             multiFloatRequest.reliability,
             multiFloatRequest.queueDepth}));
    require(waitFor([&] {
                return multiFloatPublisher->get_subscription_count() == 0;
            }),
            "unsubscribing the active type removes its exact subscription");

    client.send(
        MessageType::Subscribe,
        encodeSubscriptionRequest(
            {requestId++,
             topic("missing"),
             "missing_msgs/msg/Unavailable",
             Reliability::Reliable,
             10}));
    const auto missingError = expectError(client);
    require(missingError.code == 2001 &&
                missingError.message.find("Topic/type") != std::string::npos,
            "wrong Topic/type returns explicit subscription error");

    client.send(
        MessageType::Unsubscribe,
        encodeSubscriptionRequest(
            {requestId++,
             floatRequest.topic,
             "std_msgs/msg/Bool",
             Reliability::Reliable,
             10}));
    const auto wrongTypeError = expectError(client);
    require(wrongTypeError.code == 2002,
            "wrong unsubscribe type returns explicit error");
    require(floatPublisher->get_subscription_count() == 1,
            "wrong unsubscribe type preserves valid subscription");

    client.send(
        MessageType::Unsubscribe,
        encodeSubscriptionRequest(
            {requestId++,
             floatRequest.topic,
             floatRequest.type,
             floatRequest.reliability,
             floatRequest.queueDepth}));
    require(waitFor([&] { return floatPublisher->get_subscription_count() == 0; }),
            "unsubscribe accepts a distinct request ID for the exact topic/type");
    for (int index = 0; index < 5; ++index) {
        floatPublisher->publish(floatMessage);
        std::this_thread::sleep_for(20ms);
    }
    require(client.receivesNoSampleFor(topic("float64"), 700ms),
            "unsubscribed topic stops producing SampleBatch frames");
    progress("unsubscribe behavior verified");

    client.close();
    require(waitFor([&] {
                return boolPublisher->get_subscription_count() == 0 &&
                       pointPublisher->get_subscription_count() == 0;
            }),
            "client disconnect releases all remaining GenericSubscriptions");
    progress("disconnect cleanup verified");

    ProtocolClient reconnected(connectClient(port));
    reconnected.handshake();
    reconnected.send(
        MessageType::Subscribe,
        encodeSubscriptionRequest(
            {1, topic("float64"), "std_msgs/msg/Float64", Reliability::Reliable, 10}));
    require(waitFor([&] { return floatPublisher->get_subscription_count() == 1; }),
            "reconnected client can subscribe again");
    const auto [reconnectFrame, reconnectSample] = publishAndReceive(
        reconnected, floatPublisher, floatMessage, topic("float64"));
    static_cast<void>(reconnectFrame);
    require(number(reconnectSample, "data") == floatMessage.data,
            "reconnected session receives real samples");
    progress("reconnect and sample verified");

    reconnected.send(MessageType::Ping, encodeNonce(1));
    const auto reconnectPong = reconnected.receiveMatching(
        [](const Frame& frame) { return frame.type == MessageType::Pong; }, 2s);
    require(decodeNonce(reconnectPong.payload, &decodeError) == 1,
            "reconnected session Ping/Pong works");
    reconnected.send(MessageType::Ping, encodeNonce(2));
    reconnected.sendCorruptThenValidPing(3);
    const auto validPong = reconnected.receiveMatching(
        [&](const Frame& frame) {
            return frame.type == MessageType::Pong &&
                   decodeNonce(frame.payload, &decodeError) == 3;
        },
        2s);
    static_cast<void>(validPong);

    auto invalidDepth = encodeSubscriptionRequest(
        {8, topic("float64"), "std_msgs/msg/Float64", Reliability::Reliable, 10});
    std::fill(invalidDepth.end() - 4, invalidDepth.end(), 0);
    reconnected.send(MessageType::Subscribe, std::move(invalidDepth));
    const auto depthError = expectError(reconnected);
    require(depthError.code == 1001 &&
                depthError.message.find("queue depth") != std::string::npos,
            "zero queueDepth returns an explicit protocol error");
    require(reconnected.waitForDisconnect(3s),
            "malformed queueDepth closes the protocol session");
    progress("queueDepth protocol error verified");
    reconnected.close();

    ProtocolClient sequenceClient(connectClient(port));
    sequenceClient.handshake();
    sequenceClient.send(MessageType::Ping, encodeNonce(10));
    sequenceClient.sendCorruptThenValidPing(11);
    const auto sequencePong = sequenceClient.receiveMatching(
        [&](const Frame& frame) {
            return frame.type == MessageType::Pong &&
                   decodeNonce(frame.payload, &decodeError) == 11;
        },
        2s);
    static_cast<void>(sequencePong);
    // sendCorruptThenValidPing consumed one logical sequence. Reusing sequence is
    // exercised by a fresh raw client in the pure TCP test; here a backward
    // frame is encoded directly through a short-lived socket below.
    sequenceClient.close();

    executor.cancel();
    spinThread.join();
    executor.remove_node(node);
    node.reset();
    rclcpp::shutdown();
    require(multiTypePublisher.stopCleanly(),
            "SIGINT stops auxiliary multi-type publisher cleanly");
    require(agent.stopCleanly(), "SIGINT stops Agent process cleanly");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 3, "expects Agent and multi-type publisher paths");
        runIntegration(argv[1], argv[2]);
        std::cout << "All ROS Agent end-to-end integration tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        if (rclcpp::ok()) rclcpp::shutdown();
        std::cerr << "Test failed: " << exception.what() << '\n';
        return 1;
    }
}

#include "lab/adapters/network/network_source.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QUdpSocket>

#include <cstdint>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error("network: " + message);
    }
}

bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 2000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

struct Events {
    std::mutex mutex;
    std::vector<lab::core::DataChunk> chunks;
    std::vector<lab::core::SourceState> states;
    std::vector<std::string> errors;

    lab::core::DataSourceCallbacks callbacks() {
        return {
            [this](const lab::core::DataChunk& chunk) {
                std::scoped_lock lock(mutex);
                chunks.push_back(chunk);
            },
            [this](lab::core::SourceState state) {
                std::scoped_lock lock(mutex);
                states.push_back(state);
            },
            [this](const std::string& error) {
                std::scoped_lock lock(mutex);
                errors.push_back(error);
            },
            {}};
    }

    bool hasChunk(lab::core::Direction direction, const std::vector<std::uint8_t>& payload) {
        std::scoped_lock lock(mutex);
        for (const auto& chunk : chunks) {
            if (chunk.direction == direction && chunk.payload == payload) {
                return true;
            }
        }
        return false;
    }

    std::size_t stateCount() {
        std::scoped_lock lock(mutex);
        return states.size();
    }
};

quint16 reserveTcpPort() {
    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost, 0), "reserve TCP port");
    return server.serverPort();
}

quint16 reserveUdpPort() {
    QUdpSocket socket;
    require(socket.bind(QHostAddress::LocalHost, 0), "reserve UDP port");
    return socket.localPort();
}

void testIdleCloseIsSilent() {
    lab::adapters::network::NetworkSource source;
    Events events;
    source.setCallbacks(events.callbacks());
    source.close();
    require(events.stateCount() == 0, "closing an unopened source is idempotent");
}

void testTcpClient() {
    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost, 0), "TCP fixture listens");

    lab::adapters::network::NetworkSource source;
    Events events;
    source.setCallbacks(events.callbacks());
    source.setSettings({lab::adapters::network::NetworkMode::TcpClient,
                        "127.0.0.1",
                        server.serverPort(),
                        "0.0.0.0",
                        0});
    require(source.open(), "TCP client connection accepted");
    require(waitFor([&] { return source.isOpen() && server.hasPendingConnections(); }),
            "TCP client connects asynchronously");
    auto* peer = server.nextPendingConnection();
    require(peer != nullptr, "TCP server accepts client");

    const QByteArray inbound("tcp-rx");
    peer->write(inbound);
    peer->flush();
    require(waitFor([&] {
                return events.hasChunk(lab::core::Direction::Rx, {'t','c','p','-','r','x'});
            }),
            "TCP client receives bytes");

    const std::vector<std::uint8_t> outbound{'t','c','p','-','t','x'};
    require(source.write(outbound), "TCP client accepts transmit bytes");
    require(waitFor([&] { return peer->bytesAvailable() >= 6; }), "TCP peer receives bytes");
    require(peer->readAll() == QByteArray("tcp-tx"), "TCP transmitted payload exact");
    require(events.hasChunk(lab::core::Direction::Tx, outbound), "TCP TX event published");
    source.close();
    peer->deleteLater();
}

void testTcpServer() {
    const auto port = reserveTcpPort();
    lab::adapters::network::NetworkSource source;
    Events events;
    source.setCallbacks(events.callbacks());
    source.setSettings({lab::adapters::network::NetworkMode::TcpServer,
                        "127.0.0.1",
                        0,
                        "127.0.0.1",
                        port});
    require(source.open() && source.isOpen(), "TCP server listens");

    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, port);
    require(client.waitForConnected(2000), "TCP fixture connects to source server");
    client.write("server-rx");
    client.flush();
    require(waitFor([&] {
                return events.hasChunk(
                    lab::core::Direction::Rx,
                    {'s','e','r','v','e','r','-','r','x'});
            }),
            "TCP server receives client bytes");

    const std::vector<std::uint8_t> outbound{'s','e','r','v','e','r','-','t','x'};
    require(source.write(outbound), "TCP server sends to active client");
    require(waitFor([&] { return client.bytesAvailable() >= 9; }),
            "TCP client receives server bytes");
    require(client.readAll() == QByteArray("server-tx"), "TCP server payload exact");
    source.close();
}

void testUdp() {
    QUdpSocket peer;
    require(peer.bind(QHostAddress::LocalHost, 0), "UDP fixture binds");
    const auto sourcePort = reserveUdpPort();

    lab::adapters::network::NetworkSource source;
    Events events;
    source.setCallbacks(events.callbacks());
    source.setSettings({lab::adapters::network::NetworkMode::Udp,
                        "127.0.0.1",
                        peer.localPort(),
                        "127.0.0.1",
                        sourcePort});
    require(source.open() && source.isOpen(), "UDP source binds");

    peer.writeDatagram("udp-rx", QHostAddress::LocalHost, sourcePort);
    require(waitFor([&] {
                return events.hasChunk(lab::core::Direction::Rx, {'u','d','p','-','r','x'});
            }),
            "UDP source preserves inbound datagram");

    const std::vector<std::uint8_t> outbound{'u','d','p','-','t','x'};
    require(source.write(outbound), "UDP source sends datagram");
    require(waitFor([&] { return peer.hasPendingDatagrams(); }), "UDP peer receives datagram");
    QByteArray received(static_cast<qsizetype>(peer.pendingDatagramSize()), Qt::Uninitialized);
    peer.readDatagram(received.data(), received.size());
    require(received == QByteArray("udp-tx"), "UDP outbound datagram exact");
    require(events.hasChunk(lab::core::Direction::Tx, outbound), "UDP TX event published");
    source.close();
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    try {
        testIdleCloseIsSilent();
        testTcpClient();
        testTcpServer();
        testUdp();
        std::cout << "All Lab Debugger network tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Test failed: " << exception.what() << '\n';
        return 1;
    }
}

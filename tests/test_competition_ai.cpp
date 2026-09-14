#include "competition/deepseek_client.hpp"
#include "competition/session_summary.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QUuid>

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("competition AI: " + message);
}

bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 3000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

void writeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        throw std::runtime_error(
            "competition AI: opens fixture file " + path.toStdString() +
            ": " + file.errorString().toStdString());
    }
    require(file.write(bytes) == bytes.size(), "writes fixture file");
}

class MockDeepSeekServer final : public QObject {
public:
    MockDeepSeekServer() {
        connect(&server_, &QTcpServer::newConnection, this, [this] {
            socket_ = server_.nextPendingConnection();
            connect(socket_, &QTcpSocket::readyRead, this, [this] {
                request_ += socket_->readAll();
                const auto headerEnd = request_.indexOf("\r\n\r\n");
                if (headerEnd < 0 || replied_) return;
                const auto headers = request_.left(headerEnd);
                qint64 contentLength = -1;
                for (const auto& line : headers.split('\n')) {
                    if (line.toLower().startsWith("content-length:")) {
                        bool valid = false;
                        contentLength = line.mid(line.indexOf(':') + 1)
                                            .trimmed()
                                            .toLongLong(&valid);
                        if (!valid) contentLength = -1;
                    }
                }
                if (contentLength < 0 ||
                    request_.size() < headerEnd + 4 + contentLength) {
                    return;
                }

                const QJsonObject report{
                    {QStringLiteral("overview"), QStringLiteral("传感器异常")},
                    {QStringLiteral("findings"), QJsonArray{}},
                    {QStringLiteral("risks"), QJsonArray{}},
                    {QStringLiteral("next_actions"),
                     QJsonArray{QStringLiteral("检查温度探头")}}};
                const auto reportText = QString::fromUtf8(
                    QJsonDocument(report).toJson(QJsonDocument::Compact));
                const QJsonObject response{
                    {QStringLiteral("choices"),
                     QJsonArray{QJsonObject{
                         {QStringLiteral("message"),
                          QJsonObject{{QStringLiteral("content"), reportText}}}}}},
                    {QStringLiteral("usage"),
                     QJsonObject{{QStringLiteral("prompt_tokens"), 120},
                                 {QStringLiteral("completion_tokens"), 35}}}};
                const auto body = QJsonDocument(response).toJson(
                    QJsonDocument::Compact);
                QByteArray http = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n";
                http += "Content-Length: " + QByteArray::number(body.size()) +
                        "\r\nConnection: close\r\n\r\n" + body;
                replied_ = true;
                socket_->write(http);
                socket_->disconnectFromHost();
            });
        });
        require(server_.listen(QHostAddress::LocalHost, 0),
                "mock server listens");
    }

    [[nodiscard]] quint16 port() const { return server_.serverPort(); }
    [[nodiscard]] bool replied() const { return replied_; }
    [[nodiscard]] QByteArray request() const { return request_; }

private:
    QTcpServer server_;
    QTcpSocket* socket_{};
    QByteArray request_;
    bool replied_{};
};

void testSummaryBuilder(QByteArray& summary) {
    const auto path = QDir::current().filePath(
        QStringLiteral("lab-ai-summary-") +
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    require(QDir().mkpath(path), "temporary Session directory is available");
    const QDir directory(path);
    writeFile(
        directory.filePath(QStringLiteral("metadata.json")),
        R"({"format":"lab-debug-session","format_version":1,"status":"completed","software_version":"0.22.0","start_time_ns":1700000000123456789,"end_time_ns":1700000003123456789,"sources":[{"id":"ros-agent:demo:/temperature","type":"ros_remote_agent","name":"demo"}]})");
    writeFile(
        directory.filePath(QStringLiteral("values.csv")),
        "timestamp_ns,source_id,sequence,field,value,unit,source_timestamp_ns,receive_timestamp_ns\n"
        "1000000000,ros-agent:demo:/temperature,1,temperature,20,C,1000000000,1001000000\n"
        "2000000000,ros-agent:demo:/temperature,2,temperature,24,C,2000000000,2001000000\n"
        "3000000000,ros-agent:demo:/temperature,3,temperature,28,C,3000000000,3001000000\n"
        "3000000000,ros-agent:demo:/temperature,3,\"voltage,filtered\",12,V,3000000000,3001000000\n");
    writeFile(
        directory.filePath(QStringLiteral("events.jsonl")),
        "{\"timestamp_ns\":2500000000,\"source_id\":\"ros-agent:demo:/temperature\",\"severity\":\"warning\",\"category\":\"alert\",\"message\":\"temperature high\"}\n");

    const auto result = lab::competition::SessionSummaryBuilder::build(
        directory.absolutePath());
    require(result.success && result.valueRowsScanned == 4 &&
                result.malformedRows == 0 && !result.truncated,
            "summary scans bounded numeric input");
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(result.json, &parseError);
    require(parseError.error == QJsonParseError::NoError && document.isObject(),
            "summary is valid JSON");
    const auto root = document.object();
    const auto fields = root.value(QStringLiteral("fields")).toArray();
    require(fields.size() == 2, "summary contains both numeric fields");
    QJsonObject temperature;
    for (const auto& field : fields) {
        if (field.toObject().value(QStringLiteral("field")).toString() ==
            QStringLiteral("temperature")) {
            temperature = field.toObject();
        }
    }
    require(!temperature.isEmpty() &&
                temperature.value(QStringLiteral("count")).toInt() == 3 &&
                temperature.value(QStringLiteral("min")).toDouble() == 20.0 &&
                temperature.value(QStringLiteral("max")).toDouble() == 28.0 &&
                temperature.value(QStringLiteral("mean")).toDouble() == 24.0,
            "summary calculates reproducible field statistics");
    const auto session = root.value(QStringLiteral("session")).toObject();
    require(session.value(QStringLiteral("start_time_ns")).toString() ==
                QStringLiteral("1700000000123456789") &&
                session.value(QStringLiteral("sources"))
                        .toArray()
                        .at(0)
                        .toObject()
                        .value(QStringLiteral("name"))
                        .toString() == QStringLiteral("demo"),
            "summary preserves exact Session time and source identity");
    bool quotedFieldFound = false;
    for (const auto& field : fields) {
        quotedFieldFound = quotedFieldFound ||
            field.toObject().value(QStringLiteral("field")).toString() ==
                QStringLiteral("voltage,filtered");
    }
    require(quotedFieldFound &&
                root.value(QStringLiteral("events")).toArray().size() == 1 &&
                result.json.contains("不含原始 CDR") &&
                !result.json.contains("Authorization"),
            "summary parses quoted CSV and includes bounded non-secret evidence");
    lab::competition::SessionSummaryLimits smallOutput;
    smallOutput.maximumOutputBytes = 128;
    require(!lab::competition::SessionSummaryBuilder::build(
                 directory.absolutePath(), smallOutput)
                 .success,
            "summary output limit is enforced");
    summary = result.json;
    require(QDir(path).removeRecursively(), "temporary Session is removed");
}

void testDeepSeekClient(const QByteArray& summary) {
    MockDeepSeekServer server;
    lab::competition::DeepSeekClient client;
    QString report;
    QString failure;
    QVariantMap usage;
    client.setCallbacks(
        {},
        [&](const QString& value, const QVariantMap& tokens) {
            report = value;
            usage = tokens;
        },
        [&](const QString& message) { failure = message; });

    lab::competition::DeepSeekRequest request;
    request.endpoint = QUrl(
        QStringLiteral("http://127.0.0.1:%1/chat/completions").arg(server.port()));
    request.apiKey = QStringLiteral("test-only-key");
    request.model = QStringLiteral("deepseek-v4-flash");
    request.sessionSummaryJson = summary;
    request.experimentContext = QStringLiteral("ROS2 温度传感器升温实验");
    require(client.start(request), "local mock API request starts");
    require(waitFor([&] { return !report.isEmpty() || !failure.isEmpty(); }),
            "local mock API request completes");
    require(failure.isEmpty() && report.contains(QStringLiteral("传感器异常")) &&
                usage.value(QStringLiteral("prompt_tokens")).toInt() == 120,
            "client parses structured report and usage");
    require(server.replied(), "mock server handled one request");

    const auto rawRequest = server.request();
    require(rawRequest.startsWith("POST /chat/completions HTTP/1.1") &&
                rawRequest.contains("Authorization: Bearer test-only-key"),
            "request uses the expected endpoint and bearer header");
    const auto bodyStart = rawRequest.indexOf("\r\n\r\n");
    require(bodyStart > 0, "request contains an HTTP body");
    const auto body = QJsonDocument::fromJson(rawRequest.mid(bodyStart + 4)).object();
    require(body.value(QStringLiteral("model")).toString() ==
                QStringLiteral("deepseek-v4-flash") &&
                !body.value(QStringLiteral("stream")).toBool(true) &&
                body.value(QStringLiteral("response_format"))
                        .toObject()
                        .value(QStringLiteral("type"))
                        .toString() == QStringLiteral("json_object") &&
                body.value(QStringLiteral("thinking"))
                        .toObject()
                        .value(QStringLiteral("type"))
                        .toString() == QStringLiteral("disabled") &&
                !rawRequest.mid(bodyStart + 4).contains("test-only-key"),
            "request is low-cost JSON mode and does not put the key in its body");

    failure.clear();
    request.endpoint = QUrl(QStringLiteral("http://example.com/chat/completions"));
    require(!client.start(request) && failure.contains(QStringLiteral("HTTPS")),
            "non-local plain HTTP endpoint is rejected before transmission");
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    try {
        QByteArray summary;
        testSummaryBuilder(summary);
        testDeepSeekClient(summary);
        std::cout << "Lab Debugger competition AI tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Lab Debugger competition AI tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}

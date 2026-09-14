#include "competition/deepseek_client.hpp"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

#include <algorithm>
#include <utility>

namespace lab::competition {
namespace {

constexpr qsizetype maximumSummaryBytes = 96 * 1024;
constexpr qsizetype maximumResponseBytes = 2 * 1024 * 1024;

QString systemPrompt() {
    return QStringLiteral(
        "你是机器人与嵌入式实验数据诊断助手。只能依据用户提供的本地统计摘要作答，"
        "不得假装看过原始波形、CDR 或未提供的数据。请输出中文 JSON，严格使用结构："
        "{\"overview\":\"...\",\"findings\":[{\"phenomenon\":\"...\","
        "\"evidence\":[\"字段/数值/时间范围\"],\"possible_causes\":[\"...\"],"
        "\"verification_steps\":[\"...\"],\"confidence\":\"高/中/低\"}],"
        "\"risks\":[\"...\"],\"next_actions\":[\"...\"]}。"
        "事实、推断和建议必须分开；证据不足时明确写证据不足，不得编造故障。"
        "不要输出 Markdown 代码块，只输出一个 JSON 对象。");
}

bool endpointAllowed(const QUrl& endpoint) {
    if (!endpoint.isValid() || endpoint.host().isEmpty()) return false;
    if (endpoint.scheme() == QStringLiteral("https")) return true;
    if (endpoint.scheme() != QStringLiteral("http")) return false;
    const QHostAddress address(endpoint.host());
    return endpoint.host().compare(QStringLiteral("localhost"),
                                   Qt::CaseInsensitive) == 0 ||
           address.isLoopback();
}

QString apiErrorMessage(const QByteArray& payload) {
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return {};
    }
    const auto error = document.object().value(QStringLiteral("error"));
    if (error.isObject()) {
        return error.toObject().value(QStringLiteral("message")).toString();
    }
    if (error.isString()) return error.toString();
    return {};
}

}  // namespace

DeepSeekClient::DeepSeekClient(QObject* parent)
    : QObject(parent), network_(new QNetworkAccessManager(this)) {}

bool DeepSeekClient::isBusy() const noexcept {
    return !activeReply_.isNull();
}

void DeepSeekClient::setCallbacks(BusyHandler busy,
                                  FinishedHandler finished,
                                  FailureHandler failed) {
    busyHandler_ = std::move(busy);
    finishedHandler_ = std::move(finished);
    failureHandler_ = std::move(failed);
}

bool DeepSeekClient::start(const DeepSeekRequest& request) {
    if (isBusy()) {
        notifyFailure(tr("已有 DeepSeek 请求正在进行"));
        return false;
    }
    if (!endpointAllowed(request.endpoint)) {
        notifyFailure(tr("API 地址必须使用 HTTPS；测试时仅允许本机 HTTP"));
        return false;
    }
    if (request.apiKey.trimmed().isEmpty() || request.apiKey.size() > 4096) {
        notifyFailure(tr("请输入有效的 DeepSeek API Key"));
        return false;
    }
    if (request.model != QStringLiteral("deepseek-v4-flash") &&
        request.model != QStringLiteral("deepseek-v4-pro")) {
        notifyFailure(tr("比赛版只允许选择已核对的 DeepSeek 模型"));
        return false;
    }
    if (request.sessionSummaryJson.isEmpty() ||
        request.sessionSummaryJson.size() > maximumSummaryBytes) {
        notifyFailure(tr("Session 摘要为空或超过 96 KiB"));
        return false;
    }
    QJsonParseError summaryError;
    const auto summary = QJsonDocument::fromJson(request.sessionSummaryJson,
                                                  &summaryError);
    if (summaryError.error != QJsonParseError::NoError || !summary.isObject()) {
        notifyFailure(tr("Session 摘要不是有效 JSON"));
        return false;
    }

    const auto context = request.experimentContext.trimmed().left(2000);
    const auto userPrompt = QStringLiteral(
                                "实验背景（可能为空）：\n%1\n\n"
                                "以下是 Lab Debugger 在本地生成的 Session 聚合摘要 JSON：\n%2")
                                .arg(context,
                                     QString::fromUtf8(request.sessionSummaryJson));
    QJsonArray messages;
    messages.push_back(QJsonObject{{QStringLiteral("role"),
                                    QStringLiteral("system")},
                                   {QStringLiteral("content"), systemPrompt()}});
    messages.push_back(QJsonObject{{QStringLiteral("role"),
                                    QStringLiteral("user")},
                                   {QStringLiteral("content"), userPrompt}});
    QJsonObject body;
    body.insert(QStringLiteral("model"), request.model);
    body.insert(QStringLiteral("messages"), messages);
    body.insert(QStringLiteral("stream"), false);
    body.insert(QStringLiteral("max_tokens"),
                std::clamp(request.maximumOutputTokens, 256, 4096));
    body.insert(QStringLiteral("thinking"),
                QJsonObject{{QStringLiteral("type"),
                             QStringLiteral("disabled")}});
    body.insert(QStringLiteral("response_format"),
                QJsonObject{{QStringLiteral("type"),
                             QStringLiteral("json_object")}});

    QNetworkRequest networkRequest(request.endpoint);
    networkRequest.setHeader(QNetworkRequest::ContentTypeHeader,
                             QStringLiteral("application/json"));
    networkRequest.setRawHeader(
        "Authorization",
        QByteArrayLiteral("Bearer ") + request.apiKey.trimmed().toUtf8());
    networkRequest.setRawHeader("User-Agent", "LabDebugger-Competition/0.22");
    networkRequest.setTransferTimeout(
        std::clamp(request.timeoutMilliseconds, 5'000, 120'000));

    cancelled_ = false;
    activeReply_ = network_->post(
        networkRequest, QJsonDocument(body).toJson(QJsonDocument::Compact));
    auto* reply = activeReply_.data();
    connect(reply, &QNetworkReply::finished, this,
            [this, reply] { finishReply(reply); });
    notifyBusy(true);
    return true;
}

void DeepSeekClient::cancel() {
    if (!activeReply_) return;
    cancelled_ = true;
    activeReply_->abort();
}

void DeepSeekClient::notifyBusy(bool busy) {
    if (busyHandler_) busyHandler_(busy);
}

void DeepSeekClient::notifyFinished(QString report, QVariantMap usage) {
    if (finishedHandler_) finishedHandler_(std::move(report), std::move(usage));
}

void DeepSeekClient::notifyFailure(QString message) {
    if (failureHandler_) failureHandler_(std::move(message));
}

void DeepSeekClient::finishReply(QNetworkReply* reply) {
    if (reply != activeReply_) {
        reply->deleteLater();
        return;
    }
    activeReply_.clear();
    notifyBusy(false);

    const auto status = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto networkError = reply->error();
    auto payload = reply->readAll();
    const auto networkMessage = reply->errorString();
    reply->deleteLater();

    if (cancelled_) {
        cancelled_ = false;
        notifyFailure(tr("DeepSeek 请求已取消"));
        return;
    }
    if (payload.size() > maximumResponseBytes) {
        notifyFailure(tr("DeepSeek 响应超过 2 MiB 限制"));
        return;
    }
    if (networkError != QNetworkReply::NoError || status < 200 || status >= 300) {
        const auto apiMessage = apiErrorMessage(payload).left(1000);
        notifyFailure(
            apiMessage.isEmpty()
                ? tr("DeepSeek 请求失败（HTTP %1）：%2")
                      .arg(status)
                      .arg(networkMessage)
                : tr("DeepSeek 请求失败（HTTP %1）：%2")
                      .arg(status)
                      .arg(apiMessage));
        return;
    }

    QJsonParseError parseError;
    const auto response = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !response.isObject()) {
        notifyFailure(tr("DeepSeek 返回了无效 JSON 响应"));
        return;
    }
    const auto root = response.object();
    const auto choices = root.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        notifyFailure(tr("DeepSeek 响应中没有分析结果"));
        return;
    }
    const auto content = choices.at(0)
                             .toObject()
                             .value(QStringLiteral("message"))
                             .toObject()
                             .value(QStringLiteral("content"))
                             .toString()
                             .trimmed();
    if (content.isEmpty()) {
        notifyFailure(tr("DeepSeek 返回了空分析结果，请重试"));
        return;
    }
    QJsonParseError reportError;
    const auto reportDocument = QJsonDocument::fromJson(content.toUtf8(),
                                                         &reportError);
    if (reportError.error != QJsonParseError::NoError ||
        !reportDocument.isObject()) {
        notifyFailure(tr("DeepSeek 未按要求返回 JSON 诊断报告"));
        return;
    }
    notifyFinished(QString::fromUtf8(
                       reportDocument.toJson(QJsonDocument::Indented)),
                   root.value(QStringLiteral("usage"))
                       .toObject()
                       .toVariantMap());
}

}  // namespace lab::competition

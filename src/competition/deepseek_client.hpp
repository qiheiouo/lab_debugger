#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QVariantMap>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

namespace lab::competition {

struct DeepSeekRequest {
    QUrl endpoint{QStringLiteral("https://api.deepseek.com/chat/completions")};
    QString apiKey;
    QString model{QStringLiteral("deepseek-v4-flash")};
    QByteArray sessionSummaryJson;
    QString experimentContext;
    int maximumOutputTokens{1400};
    int timeoutMilliseconds{45'000};
};

class DeepSeekClient final : public QObject {
public:
    using BusyHandler = std::function<void(bool)>;
    using FinishedHandler = std::function<void(QString, QVariantMap)>;
    using FailureHandler = std::function<void(QString)>;

    explicit DeepSeekClient(QObject* parent = nullptr);

    [[nodiscard]] bool isBusy() const noexcept;
    [[nodiscard]] bool start(const DeepSeekRequest& request);
    void setCallbacks(BusyHandler busy,
                      FinishedHandler finished,
                      FailureHandler failed);

    void cancel();

private:
    void notifyBusy(bool busy);
    void notifyFinished(QString report, QVariantMap usage);
    void notifyFailure(QString message);
    void finishReply(QNetworkReply* reply);

    QNetworkAccessManager* network_{};
    QPointer<QNetworkReply> activeReply_;
    bool cancelled_{};
    BusyHandler busyHandler_;
    FinishedHandler finishedHandler_;
    FailureHandler failureHandler_;
};

}  // namespace lab::competition

#pragma once

#include "lab/adapters/remote_agent/remote_agent_source.hpp"

#include <QVariantList>
#include <QWidget>

#include <cstdint>
#include <optional>

class QLabel;
class QCheckBox;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace lab::ui {

class RemoteAgentPanel final : public QWidget {
    Q_OBJECT

public:
    explicit RemoteAgentPanel(QWidget* parent = nullptr);

    [[nodiscard]] lab::adapters::remote_agent::RemoteAgentSettings settings() const;

signals:
    void connectRequested();
    void disconnectRequested();
    void reconnectRequested();
    void refreshTopicsRequested();
    void subscribeRequested(lab::core::agent::SubscriptionRequest request);
    void unsubscribeRequested(lab::core::agent::SubscriptionRequest request);

public slots:
    void setSourceState(int state);
    void setAgentHello(QString agentId,
                       QString softwareVersion,
                       QString hostName,
                       quint32 capabilities);
    void setTopics(QVariantList topics, quint64 graphRevision);

private slots:
    void updateTopicButtons();
    void requestSubscribe();
    void requestUnsubscribe();

private:
    [[nodiscard]] std::optional<lab::core::agent::SubscriptionRequest>
    currentRequest();

    QLineEdit* host_{};
    QSpinBox* port_{};
    QLineEdit* clientName_{};
    QCheckBox* autoReconnect_{};
    QLabel* state_{};
    QLabel* agentIdentity_{};
    QLabel* catalogRevision_{};
    QTableWidget* topics_{};
    QSpinBox* queueDepth_{};
    QPushButton* connectButton_{};
    QPushButton* disconnectButton_{};
    QPushButton* reconnectButton_{};
    QPushButton* refreshButton_{};
    QPushButton* subscribeButton_{};
    QPushButton* unsubscribeButton_{};
    std::uint64_t nextRequestId_{1};
    bool ready_{};
};

}  // namespace lab::ui

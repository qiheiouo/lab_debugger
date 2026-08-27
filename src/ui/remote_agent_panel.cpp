#include "ui/remote_agent_panel.hpp"

#include "lab/core/data_source.hpp"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <utility>

namespace lab::ui {
namespace {

QString reliabilityName(int value) {
    switch (static_cast<lab::core::agent::Reliability>(value)) {
    case lab::core::agent::Reliability::BestEffort: return QStringLiteral("Best effort");
    case lab::core::agent::Reliability::Reliable: return QStringLiteral("Reliable");
    case lab::core::agent::Reliability::Unknown: return QStringLiteral("Unknown");
    }
    return QStringLiteral("Unknown");
}

QString durabilityName(int value) {
    switch (static_cast<lab::core::agent::Durability>(value)) {
    case lab::core::agent::Durability::Volatile: return QStringLiteral("Volatile");
    case lab::core::agent::Durability::TransientLocal:
        return QStringLiteral("Transient local");
    case lab::core::agent::Durability::Unknown: return QStringLiteral("Unknown");
    }
    return QStringLiteral("Unknown");
}

}  // namespace

RemoteAgentPanel::RemoteAgentPanel(QWidget* parent) : QWidget(parent) {
    auto* title = new QLabel(tr("ROS 2 Remote Agent"), this);
    auto titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 2);
    title->setFont(titleFont);

    host_ = new QLineEdit(QStringLiteral("127.0.0.1"), this);
    port_ = new QSpinBox(this);
    port_->setRange(1, 65535);
    port_->setValue(9750);
    clientName_ = new QLineEdit(QStringLiteral("Lab Debugger"), this);
    autoReconnect_ = new QCheckBox(tr("掉线后自动重连并恢复订阅"), this);
    autoReconnect_->setChecked(true);

    auto* form = new QFormLayout;
    form->addRow(tr("Agent 地址"), host_);
    form->addRow(tr("Agent 端口"), port_);
    form->addRow(tr("客户端名称"), clientName_);
    form->addRow(tr("连接恢复"), autoReconnect_);

    state_ = new QLabel(tr("● 未连接"), this);
    state_->setStyleSheet(QStringLiteral("color: #9aa4b2;"));
    agentIdentity_ = new QLabel(tr("尚未收到 Agent 身份"), this);
    agentIdentity_->setWordWrap(true);
    agentIdentity_->setObjectName(QStringLiteral("secondaryText"));

    connectButton_ = new QPushButton(tr("连接"), this);
    disconnectButton_ = new QPushButton(tr("断开"), this);
    reconnectButton_ = new QPushButton(tr("重连"), this);
    disconnectButton_->setEnabled(false);
    reconnectButton_->setEnabled(false);
    auto* connectionButtons = new QHBoxLayout;
    connectionButtons->addWidget(connectButton_);
    connectionButtons->addWidget(disconnectButton_);
    connectionButtons->addWidget(reconnectButton_);

    catalogRevision_ = new QLabel(tr("Topic 目录：未获取"), this);
    refreshButton_ = new QPushButton(tr("刷新 Topic"), this);
    refreshButton_->setEnabled(false);
    auto* catalogHeader = new QHBoxLayout;
    catalogHeader->addWidget(catalogRevision_, 1);
    catalogHeader->addWidget(refreshButton_);

    topics_ = new QTableWidget(0, 4, this);
    topics_->setHorizontalHeaderLabels(
        {tr("Topic"), tr("消息类型"), tr("可靠性"), tr("持久性")});
    topics_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    topics_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    topics_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    topics_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    topics_->verticalHeader()->hide();
    topics_->setSelectionBehavior(QAbstractItemView::SelectRows);
    topics_->setSelectionMode(QAbstractItemView::SingleSelection);
    topics_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    topics_->setMinimumHeight(180);

    queueDepth_ = new QSpinBox(this);
    queueDepth_->setRange(1, 1000000);
    queueDepth_->setValue(10);
    subscribeButton_ = new QPushButton(tr("订阅选中 Topic"), this);
    unsubscribeButton_ = new QPushButton(tr("取消订阅"), this);
    subscribeButton_->setEnabled(false);
    unsubscribeButton_->setEnabled(false);
    auto* subscriptionRow = new QHBoxLayout;
    subscriptionRow->addWidget(new QLabel(tr("队列深度"), this));
    subscriptionRow->addWidget(queueDepth_);
    subscriptionRow->addWidget(subscribeButton_);
    subscriptionRow->addWidget(unsubscribeButton_);

    auto* hint = new QLabel(
        tr("Agent 握手完成前不会进入已连接状态。原始 CDR 写入 Session，"
           "数值与布尔字段直接进入实时曲线。"),
        this);
    hint->setWordWrap(true);
    hint->setObjectName(QStringLiteral("secondaryText"));

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(title);
    layout->addLayout(form);
    layout->addWidget(state_);
    layout->addWidget(agentIdentity_);
    layout->addLayout(connectionButtons);
    layout->addSpacing(8);
    layout->addLayout(catalogHeader);
    layout->addWidget(topics_, 1);
    layout->addLayout(subscriptionRow);
    layout->addWidget(hint);

    connect(connectButton_, &QPushButton::clicked,
            this, &RemoteAgentPanel::connectRequested);
    connect(disconnectButton_, &QPushButton::clicked,
            this, &RemoteAgentPanel::disconnectRequested);
    connect(reconnectButton_, &QPushButton::clicked,
            this, &RemoteAgentPanel::reconnectRequested);
    connect(refreshButton_, &QPushButton::clicked,
            this, &RemoteAgentPanel::refreshTopicsRequested);
    connect(topics_, &QTableWidget::itemSelectionChanged,
            this, &RemoteAgentPanel::updateTopicButtons);
    connect(subscribeButton_, &QPushButton::clicked,
            this, &RemoteAgentPanel::requestSubscribe);
    connect(unsubscribeButton_, &QPushButton::clicked,
            this, &RemoteAgentPanel::requestUnsubscribe);
}

lab::adapters::remote_agent::RemoteAgentSettings RemoteAgentPanel::settings() const {
    return {host_->text().trimmed().toStdString(),
            static_cast<std::uint16_t>(port_->value()),
            clientName_->text().trimmed().toStdString(),
            "0.7.0",
            autoReconnect_->isChecked()};
}

void RemoteAgentPanel::setSourceState(int rawState) {
    const auto sourceState = static_cast<lab::core::SourceState>(rawState);
    ready_ = sourceState == lab::core::SourceState::Open;
    connectButton_->setEnabled(sourceState == lab::core::SourceState::Closed ||
                               sourceState == lab::core::SourceState::Error);
    disconnectButton_->setEnabled(sourceState == lab::core::SourceState::Opening || ready_);
    reconnectButton_->setEnabled(sourceState == lab::core::SourceState::Closed ||
                                 sourceState == lab::core::SourceState::Error || ready_);
    refreshButton_->setEnabled(ready_);
    switch (sourceState) {
    case lab::core::SourceState::Opening:
        state_->setText(tr("● 正在连接、等待重试或等待 Agent 握手"));
        state_->setStyleSheet(QStringLiteral("color: #e5b567;"));
        agentIdentity_->setText(tr("等待新的 Agent 身份"));
        topics_->setRowCount(0);
        catalogRevision_->setText(tr("Topic 目录：等待握手"));
        break;
    case lab::core::SourceState::Open:
        state_->setText(tr("● Agent 已就绪"));
        state_->setStyleSheet(QStringLiteral("color: #5ad49b;"));
        break;
    case lab::core::SourceState::Closing:
        state_->setText(tr("● 正在断开"));
        state_->setStyleSheet(QStringLiteral("color: #e5b567;"));
        break;
    case lab::core::SourceState::Error:
        state_->setText(tr("● Agent 连接或协议错误"));
        state_->setStyleSheet(QStringLiteral("color: #ff6b6b;"));
        break;
    case lab::core::SourceState::Closed:
    default:
        state_->setText(tr("● 未连接"));
        state_->setStyleSheet(QStringLiteral("color: #9aa4b2;"));
        break;
    }
    updateTopicButtons();
}

void RemoteAgentPanel::setAgentHello(
    QString agentId,
    QString softwareVersion,
    QString hostName,
    quint32 capabilities) {
    agentIdentity_->setText(
        tr("Agent：%1  |  版本：%2  |  主机：%3  |  能力：0x%4")
            .arg(agentId, softwareVersion, hostName)
            .arg(capabilities, 8, 16, QLatin1Char('0')));
}

void RemoteAgentPanel::setTopics(QVariantList topics, quint64 graphRevision) {
    topics_->setRowCount(topics.size());
    for (int row = 0; row < topics.size(); ++row) {
        const auto item = topics[row].toMap();
        const auto reliability = item.value(QStringLiteral("reliability")).toInt();
        const auto durability = item.value(QStringLiteral("durability")).toInt();
        auto* name = new QTableWidgetItem(item.value(QStringLiteral("name")).toString());
        name->setData(Qt::UserRole, reliability);
        name->setData(Qt::UserRole + 1, durability);
        topics_->setItem(row, 0, name);
        topics_->setItem(
            row, 1, new QTableWidgetItem(item.value(QStringLiteral("type")).toString()));
        topics_->setItem(row, 2, new QTableWidgetItem(reliabilityName(reliability)));
        topics_->setItem(row, 3, new QTableWidgetItem(durabilityName(durability)));
    }
    catalogRevision_->setText(
        tr("Topic 目录：%1 项，图版本 %2").arg(topics.size()).arg(graphRevision));
    updateTopicButtons();
}

void RemoteAgentPanel::updateTopicButtons() {
    const auto selectable = ready_ && topics_->currentRow() >= 0;
    subscribeButton_->setEnabled(selectable);
    unsubscribeButton_->setEnabled(selectable);
}

void RemoteAgentPanel::requestSubscribe() {
    if (auto request = currentRequest()) {
        emit subscribeRequested(std::move(*request));
    }
}

void RemoteAgentPanel::requestUnsubscribe() {
    if (auto request = currentRequest()) {
        emit unsubscribeRequested(std::move(*request));
    }
}

std::optional<lab::core::agent::SubscriptionRequest>
RemoteAgentPanel::currentRequest() {
    const auto row = topics_->currentRow();
    if (!ready_ || row < 0 || !topics_->item(row, 0) || !topics_->item(row, 1)) {
        return std::nullopt;
    }
    return lab::core::agent::SubscriptionRequest{
        nextRequestId_++,
        topics_->item(row, 0)->text().toStdString(),
        topics_->item(row, 1)->text().toStdString(),
        static_cast<lab::core::agent::Reliability>(
            topics_->item(row, 0)->data(Qt::UserRole).toInt()),
        static_cast<std::uint32_t>(queueDepth_->value())};
}

}  // namespace lab::ui

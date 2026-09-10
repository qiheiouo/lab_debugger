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

QString topicFieldKey(const QString& name, const QString& type) {
    return name + QChar(0x001F) + type;
}

QString mappingName(int value, bool negotiated) {
    switch (static_cast<lab::core::agent::FieldMappingKind>(value)) {
    case lab::core::agent::FieldMappingKind::BuiltIn: return QObject::tr("内置语义");
    case lab::core::agent::FieldMappingKind::Introspection:
        return QObject::tr("通用解析");
    case lab::core::agent::FieldMappingKind::RawOnly:
        return QObject::tr("仅原始 CDR");
    case lab::core::agent::FieldMappingKind::Unavailable:
        return QObject::tr("不可订阅");
    case lab::core::agent::FieldMappingKind::Unknown:
        return negotiated ? QObject::tr("等待 Agent") : QObject::tr("Agent 未提供");
    }
    return QObject::tr("未知");
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
    clockSync_ = new QLabel(tr("时钟同步：等待样本"), this);
    clockSync_->setWordWrap(true);
    clockSync_->setObjectName(QStringLiteral("secondaryText"));

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

    topics_ = new QTableWidget(0, 5, this);
    topics_->setHorizontalHeaderLabels(
        {tr("Topic"), tr("消息类型"), tr("可靠性"), tr("持久性"), tr("字段能力")});
    topics_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    topics_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    topics_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    topics_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    topics_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
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
    layout->addWidget(clockSync_);
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
            "0.21.0",
            autoReconnect_->isChecked()};
}

void RemoteAgentPanel::setSourceState(int rawState) {
    const auto sourceState = static_cast<lab::core::SourceState>(rawState);
    ready_ = sourceState == lab::core::SourceState::Open;
    if (!ready_) {
        clockSync_->setText(tr("时钟同步：等待样本"));
    }
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
        topicFields_.clear();
        currentGraphRevision_ = 0;
        topicFieldRevision_ = 0;
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

void RemoteAgentPanel::setClockSync(
    qint64 offsetNs,
    qint64 roundTripNs,
    qint64 uncertaintyNs,
    quint32 sampleCount) {
    auto offset = QString::number(static_cast<double>(offsetNs) / 1'000'000.0, 'f', 3);
    if (offsetNs > 0) offset.prepend(QLatin1Char('+'));
    clockSync_->setText(
        tr("时钟同步：偏移 %1 ms  |  RTT %2 ms  |  不确定度 ±%3 ms  |  %4 个样本")
            .arg(offset)
            .arg(static_cast<double>(roundTripNs) / 1'000'000.0, 0, 'f', 3)
            .arg(static_cast<double>(uncertaintyNs) / 1'000'000.0, 0, 'f', 3)
            .arg(sampleCount));
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
    topicFieldCatalogNegotiated_ =
        (capabilities & lab::core::agent::capabilityMask(
                            lab::core::agent::Capability::TopicFieldCapabilities)) != 0U;
    if (!topicFieldCatalogNegotiated_) {
        topicFields_.clear();
        topicFieldRevision_ = 0;
    }
}

void RemoteAgentPanel::setTopics(QVariantList topics, quint64 graphRevision) {
    currentGraphRevision_ = graphRevision;
    if (topicFieldRevision_ != graphRevision) topicFields_.clear();
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
        updateTopicFieldCell(row);
    }
    catalogRevision_->setText(
        tr("Topic 目录：%1 项，图版本 %2").arg(topics.size()).arg(graphRevision));
    updateTopicButtons();
}

void RemoteAgentPanel::setTopicFields(
    QVariantList topics,
    quint64 graphRevision) {
    topicFields_.clear();
    topicFieldRevision_ = graphRevision;
    for (const auto& value : topics) {
        const auto item = value.toMap();
        topicFields_.insert(
            topicFieldKey(item.value(QStringLiteral("name")).toString(),
                          item.value(QStringLiteral("type")).toString()),
            item);
    }
    if (currentGraphRevision_ == graphRevision) {
        for (int row = 0; row < topics_->rowCount(); ++row) {
            updateTopicFieldCell(row);
        }
    }
    updateTopicButtons();
}

void RemoteAgentPanel::updateTopicFieldCell(int row) {
    if (!topics_->item(row, 0) || !topics_->item(row, 1)) return;
    const auto key = topicFieldKey(
        topics_->item(row, 0)->text(), topics_->item(row, 1)->text());
    const auto mapping = topicFieldRevision_ == currentGraphRevision_
                             ? topicFields_.value(key)
                             : QVariantMap{};
    const auto kind = mapping.value(
        QStringLiteral("mapping"),
        static_cast<int>(lab::core::agent::FieldMappingKind::Unknown)).toInt();
    auto* capability = new QTableWidgetItem(
        mappingName(kind, topicFieldCatalogNegotiated_));
    capability->setData(Qt::UserRole, kind);
    const auto reason = mapping.value(QStringLiteral("reason")).toString();
    if (!reason.isEmpty()) capability->setToolTip(reason);
    topics_->setItem(row, 4, capability);
}

void RemoteAgentPanel::updateTopicButtons() {
    const auto selectable = ready_ && topics_->currentRow() >= 0;
    const auto* mapping = selectable ? topics_->item(topics_->currentRow(), 4) : nullptr;
    const auto unavailable = mapping &&
        mapping->data(Qt::UserRole).toInt() ==
            static_cast<int>(lab::core::agent::FieldMappingKind::Unavailable);
    subscribeButton_->setEnabled(selectable && !unavailable);
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

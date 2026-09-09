#include "ui/serial_panel.hpp"

#include "app/serial_session.hpp"
#include "lab/core/data_source.hpp"

#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVariantMap>
#include <QVBoxLayout>

namespace lab::ui {

SerialPanel::SerialPanel(QWidget* parent) : QWidget(parent) {
    setMinimumWidth(250);
    auto* title = new QLabel(tr("串口数据源"), this);
    auto titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 2);
    title->setFont(titleFont);

    sources_ = new QListWidget(this);
    sources_->setObjectName(QStringLiteral("serialSourceList"));
    sources_->setMaximumHeight(130);

    port_ = new QComboBox(this);
    auto* refreshButton = new QPushButton(tr("刷新"), this);
    auto* portRow = new QWidget(this);
    auto* portLayout = new QHBoxLayout(portRow);
    portLayout->setContentsMargins(0, 0, 0, 0);
    portLayout->addWidget(port_, 1);
    portLayout->addWidget(refreshButton);

    baud_ = new QComboBox(this);
    baud_->setEditable(true);
    for (const auto baud : {9600, 19200, 38400, 57600, 115200, 460800, 921600}) {
        baud_->addItem(QString::number(baud), baud);
    }
    baud_->setCurrentText(QStringLiteral("115200"));

    dataBits_ = new QComboBox(this);
    for (const auto bits : {5, 6, 7, 8}) {
        dataBits_->addItem(QString::number(bits), bits);
    }
    dataBits_->setCurrentText(QStringLiteral("8"));

    stopBits_ = new QComboBox(this);
    stopBits_->addItem(tr("1"), static_cast<int>(lab::adapters::serial::StopBits::One));
    stopBits_->addItem(tr("1.5"), static_cast<int>(lab::adapters::serial::StopBits::OneAndHalf));
    stopBits_->addItem(tr("2"), static_cast<int>(lab::adapters::serial::StopBits::Two));

    parity_ = new QComboBox(this);
    parity_->addItem(tr("无"), static_cast<int>(lab::adapters::serial::Parity::None));
    parity_->addItem(tr("偶校验"), static_cast<int>(lab::adapters::serial::Parity::Even));
    parity_->addItem(tr("奇校验"), static_cast<int>(lab::adapters::serial::Parity::Odd));
    parity_->addItem(tr("Space"), static_cast<int>(lab::adapters::serial::Parity::Space));
    parity_->addItem(tr("Mark"), static_cast<int>(lab::adapters::serial::Parity::Mark));

    flowControl_ = new QComboBox(this);
    flowControl_->addItem(tr("无"), static_cast<int>(lab::adapters::serial::FlowControl::None));
    flowControl_->addItem(tr("硬件 RTS/CTS"), static_cast<int>(lab::adapters::serial::FlowControl::Hardware));
    flowControl_->addItem(tr("软件 XON/XOFF"), static_cast<int>(lab::adapters::serial::FlowControl::Software));

    auto* form = new QFormLayout;
    form->addRow(tr("端口"), portRow);
    form->addRow(tr("波特率"), baud_);
    form->addRow(tr("数据位"), dataBits_);
    form->addRow(tr("停止位"), stopBits_);
    form->addRow(tr("校验"), parity_);
    form->addRow(tr("流控"), flowControl_);

    state_ = new QLabel(tr("● 未连接"), this);
    state_->setStyleSheet(QStringLiteral("color: #9aa4b2;"));

    connectButton_ = new QPushButton(tr("添加 / 连接"), this);
    disconnectButton_ = new QPushButton(tr("断开"), this);
    reconnectButton_ = new QPushButton(tr("重连"), this);
    removeButton_ = new QPushButton(tr("移除"), this);
    disconnectButton_->setEnabled(false);
    reconnectButton_->setEnabled(false);
    removeButton_->setEnabled(false);

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(connectButton_);
    buttons->addWidget(disconnectButton_);
    buttons->addWidget(reconnectButton_);
    buttons->addWidget(removeButton_);

    auto* hint = new QLabel(
        tr("串口 I/O 在独立线程运行。暂停终端或曲线不会停止采集与记录。"),
        this);
    hint->setWordWrap(true);
    hint->setObjectName(QStringLiteral("secondaryText"));

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(title);
    layout->addWidget(new QLabel(tr("已配置串口（可同时打开）"), this));
    layout->addWidget(sources_);
    layout->addSpacing(8);
    layout->addLayout(form);
    layout->addWidget(state_);
    layout->addLayout(buttons);
    layout->addStretch(1);
    layout->addWidget(hint);

    connect(refreshButton, &QPushButton::clicked, this, &SerialPanel::refreshPorts);
    connect(connectButton_, &QPushButton::clicked, this, &SerialPanel::connectRequested);
    connect(disconnectButton_, &QPushButton::clicked, this, [this] {
        const auto sourceId = selectedSourceId();
        if (!sourceId.isEmpty()) emit disconnectSourceRequested(sourceId);
    });
    connect(reconnectButton_, &QPushButton::clicked, this, [this] {
        const auto sourceId = selectedSourceId();
        if (!sourceId.isEmpty()) emit reconnectSourceRequested(sourceId);
    });
    connect(removeButton_, &QPushButton::clicked, this, [this] {
        const auto sourceId = selectedSourceId();
        if (!sourceId.isEmpty()) emit removeSourceRequested(sourceId);
    });
    connect(sources_, &QListWidget::currentItemChanged,
            this, [this] { loadSelectedSource(); });
    refreshPorts();
}

QString SerialPanel::selectedSourceId() const {
    const auto* item = sources_->currentItem();
    return item == nullptr
               ? QString{}
               : item->data(Qt::UserRole).toMap().value(QStringLiteral("id")).toString();
}

void SerialPanel::setSources(const QVariantList& sources) {
    const auto previous = selectedSourceId();
    sources_->blockSignals(true);
    sources_->clear();
    int selectedIndex = -1;
    for (const auto& value : sources) {
        const auto source = value.toMap();
        if (source.value(QStringLiteral("type")).toString() !=
            QStringLiteral("serial")) {
            continue;
        }
        const auto id = source.value(QStringLiteral("id")).toString();
        const auto state = static_cast<lab::core::SourceState>(
            source.value(QStringLiteral("state")).toInt());
        const auto suffix = state == lab::core::SourceState::Open
                                ? tr("  [已连接]")
                            : state == lab::core::SourceState::Error
                                ? tr("  [错误]")
                                : tr("  [已断开]");
        auto* item = new QListWidgetItem(id + suffix, sources_);
        item->setData(Qt::UserRole, source);
        const auto row = sources_->row(item);
        if (id == previous ||
            (selectedIndex < 0 &&
             source.value(QStringLiteral("selected")).toBool())) {
            selectedIndex = row;
        }
    }
    if (selectedIndex < 0 && sources_->count() > 0) selectedIndex = 0;
    sources_->setCurrentRow(selectedIndex);
    sources_->blockSignals(false);
    loadSelectedSource();
}

void SerialPanel::loadSelectedSource() {
    const auto* item = sources_->currentItem();
    if (item == nullptr) {
        disconnectButton_->setEnabled(false);
        reconnectButton_->setEnabled(false);
        removeButton_->setEnabled(false);
        setSourceState(static_cast<int>(lab::core::SourceState::Closed));
        return;
    }
    const auto source = item->data(Qt::UserRole).toMap();
    const auto portName = source.value(QStringLiteral("port")).toString();
    auto portIndex = port_->findData(portName);
    if (portIndex < 0) {
        port_->addItem(portName, portName);
        portIndex = port_->count() - 1;
    }
    port_->setCurrentIndex(portIndex);
    baud_->setCurrentText(QString::number(
        source.value(QStringLiteral("baud_rate")).toInt()));
    for (auto [combo, key] : {
             std::pair<QComboBox*, QString>{dataBits_, QStringLiteral("data_bits")},
             {stopBits_, QStringLiteral("stop_bits")},
             {parity_, QStringLiteral("parity")},
             {flowControl_, QStringLiteral("flow_control")}}) {
        const auto index = combo->findData(source.value(key));
        if (index >= 0) combo->setCurrentIndex(index);
    }
    removeButton_->setEnabled(true);
    setSourceState(source.value(QStringLiteral("state")).toInt());
}

lab::adapters::serial::SerialSettings SerialPanel::settings() const {
    return {
        port_->currentData().toString().toStdString(),
        baud_->currentText().toInt(),
        dataBits_->currentData().toInt(),
        static_cast<lab::adapters::serial::StopBits>(stopBits_->currentData().toInt()),
        static_cast<lab::adapters::serial::Parity>(parity_->currentData().toInt()),
        static_cast<lab::adapters::serial::FlowControl>(flowControl_->currentData().toInt())};
}

void SerialPanel::refreshPorts() {
    const auto previous = port_->currentData().toString();
    port_->clear();
    for (const auto& info : lab::app::SerialSession::availablePorts()) {
        auto label = QString::fromStdString(info.name);
        if (!info.description.empty()) {
            label += QStringLiteral(" — ") + QString::fromStdString(info.description);
        }
        port_->addItem(label, QString::fromStdString(info.name));
    }
    const auto previousIndex = port_->findData(previous);
    if (previousIndex >= 0) {
        port_->setCurrentIndex(previousIndex);
    }
    if (port_->count() == 0) {
        port_->addItem(tr("未发现串口"), QString());
    }
}

void SerialPanel::setSourceState(int rawState) {
    const auto sourceState = static_cast<lab::core::SourceState>(rawState);
    const bool open = sourceState == lab::core::SourceState::Open;
    connectButton_->setEnabled(!port_->currentData().toString().isEmpty());
    disconnectButton_->setEnabled(open);
    reconnectButton_->setEnabled(
        sources_->currentItem() != nullptr &&
        sourceState != lab::core::SourceState::Opening &&
        sourceState != lab::core::SourceState::Closing);
    removeButton_->setEnabled(sources_->currentItem() != nullptr);

    switch (sourceState) {
    case lab::core::SourceState::Opening:
        state_->setText(tr("● 正在连接"));
        state_->setStyleSheet(QStringLiteral("color: #e5b567;"));
        break;
    case lab::core::SourceState::Open:
        state_->setText(tr("● 已连接"));
        state_->setStyleSheet(QStringLiteral("color: #5ad49b;"));
        break;
    case lab::core::SourceState::Closing:
        state_->setText(tr("● 正在断开"));
        state_->setStyleSheet(QStringLiteral("color: #e5b567;"));
        break;
    case lab::core::SourceState::Error:
        state_->setText(tr("● 连接错误"));
        state_->setStyleSheet(QStringLiteral("color: #ff6b6b;"));
        break;
    case lab::core::SourceState::Closed:
    default:
        state_->setText(tr("● 未连接"));
        state_->setStyleSheet(QStringLiteral("color: #9aa4b2;"));
        break;
    }
}

}  // namespace lab::ui

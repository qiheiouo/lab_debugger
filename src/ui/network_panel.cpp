#include "ui/network_panel.hpp"

#include "lab/core/data_source.hpp"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace lab::ui {

NetworkPanel::NetworkPanel(QWidget* parent) : QWidget(parent) {
    auto* title = new QLabel(tr("网络数据源"), this);
    auto titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 2);
    title->setFont(titleFont);

    mode_ = new QComboBox(this);
    mode_->addItem(tr("TCP 客户端"),
                   static_cast<int>(lab::adapters::network::NetworkMode::TcpClient));
    mode_->addItem(tr("TCP 服务端"),
                   static_cast<int>(lab::adapters::network::NetworkMode::TcpServer));
    mode_->addItem(tr("UDP"),
                   static_cast<int>(lab::adapters::network::NetworkMode::Udp));
    remoteHost_ = new QLineEdit(QStringLiteral("127.0.0.1"), this);
    remotePort_ = new QSpinBox(this);
    remotePort_->setRange(1, 65535);
    remotePort_->setValue(9000);
    bindAddress_ = new QLineEdit(QStringLiteral("0.0.0.0"), this);
    localPort_ = new QSpinBox(this);
    localPort_->setRange(1, 65535);
    localPort_->setValue(9000);

    auto* form = new QFormLayout;
    form->addRow(tr("模式"), mode_);
    form->addRow(tr("远端地址"), remoteHost_);
    form->addRow(tr("远端端口"), remotePort_);
    form->addRow(tr("监听地址"), bindAddress_);
    form->addRow(tr("本地端口"), localPort_);

    state_ = new QLabel(tr("● 未连接"), this);
    state_->setStyleSheet(QStringLiteral("color: #9aa4b2;"));
    connectButton_ = new QPushButton(tr("打开"), this);
    disconnectButton_ = new QPushButton(tr("关闭"), this);
    reconnectButton_ = new QPushButton(tr("重开"), this);
    disconnectButton_->setEnabled(false);
    reconnectButton_->setEnabled(false);
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(connectButton_);
    buttons->addWidget(disconnectButton_);
    buttons->addWidget(reconnectButton_);

    auto* hint = new QLabel(
        tr("TCP/UDP I/O 在独立线程运行。UDP 保留数据报边界；UDP 远端地址当前需填写 IPv4/IPv6 数字地址。"),
        this);
    hint->setWordWrap(true);
    hint->setObjectName(QStringLiteral("secondaryText"));

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(title);
    layout->addSpacing(8);
    layout->addLayout(form);
    layout->addWidget(state_);
    layout->addLayout(buttons);
    layout->addStretch();
    layout->addWidget(hint);

    connect(mode_, &QComboBox::currentIndexChanged,
            this, &NetworkPanel::updateModeControls);
    connect(connectButton_, &QPushButton::clicked, this, &NetworkPanel::connectRequested);
    connect(disconnectButton_, &QPushButton::clicked, this, &NetworkPanel::disconnectRequested);
    connect(reconnectButton_, &QPushButton::clicked, this, &NetworkPanel::reconnectRequested);
    updateModeControls();
}

lab::adapters::network::NetworkSettings NetworkPanel::settings() const {
    return {
        static_cast<lab::adapters::network::NetworkMode>(mode_->currentData().toInt()),
        remoteHost_->text().trimmed().toStdString(),
        static_cast<std::uint16_t>(remotePort_->value()),
        bindAddress_->text().trimmed().toStdString(),
        static_cast<std::uint16_t>(localPort_->value())};
}

void NetworkPanel::setSourceState(int rawState) {
    const auto sourceState = static_cast<lab::core::SourceState>(rawState);
    const bool open = sourceState == lab::core::SourceState::Open;
    connectButton_->setEnabled(!open);
    disconnectButton_->setEnabled(open || sourceState == lab::core::SourceState::Opening);
    reconnectButton_->setEnabled(open || sourceState == lab::core::SourceState::Error ||
                                 sourceState == lab::core::SourceState::Closed);
    switch (sourceState) {
    case lab::core::SourceState::Opening:
        state_->setText(tr("● 正在打开"));
        state_->setStyleSheet(QStringLiteral("color: #e5b567;"));
        break;
    case lab::core::SourceState::Open:
        state_->setText(tr("● 已打开"));
        state_->setStyleSheet(QStringLiteral("color: #5ad49b;"));
        break;
    case lab::core::SourceState::Closing:
        state_->setText(tr("● 正在关闭"));
        state_->setStyleSheet(QStringLiteral("color: #e5b567;"));
        break;
    case lab::core::SourceState::Error:
        state_->setText(tr("● 网络错误"));
        state_->setStyleSheet(QStringLiteral("color: #ff6b6b;"));
        break;
    case lab::core::SourceState::Closed:
    default:
        state_->setText(tr("● 未连接"));
        state_->setStyleSheet(QStringLiteral("color: #9aa4b2;"));
        break;
    }
}

void NetworkPanel::updateModeControls() {
    const auto mode = static_cast<lab::adapters::network::NetworkMode>(
        mode_->currentData().toInt());
    const bool usesRemote = mode != lab::adapters::network::NetworkMode::TcpServer;
    const bool usesLocal = mode != lab::adapters::network::NetworkMode::TcpClient;
    remoteHost_->setEnabled(usesRemote);
    remotePort_->setEnabled(usesRemote);
    bindAddress_->setEnabled(usesLocal);
    localPort_->setEnabled(usesLocal);
}

}  // namespace lab::ui

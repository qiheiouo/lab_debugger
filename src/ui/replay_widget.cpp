#include "ui/replay_widget.hpp"

#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

#include <algorithm>

namespace lab::ui {
namespace {

QString formatDuration(qint64 nanoseconds) {
    const auto totalMilliseconds = std::max<qint64>(0, nanoseconds / 1'000'000);
    const auto milliseconds = totalMilliseconds % 1000;
    const auto totalSeconds = totalMilliseconds / 1000;
    const auto seconds = totalSeconds % 60;
    const auto minutes = totalSeconds / 60;
    return QStringLiteral("%1:%2.%3")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'))
        .arg(milliseconds, 3, 10, QLatin1Char('0'));
}

}  // namespace

ReplayWidget::ReplayWidget(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    auto* controls = new QHBoxLayout;
    auto* openButton = new QPushButton(tr("打开 Session"), this);
    closeButton_ = new QPushButton(tr("关闭回放"), this);
    playButton_ = new QPushButton(tr("播放"), this);
    auto* speed = new QComboBox(this);
    for (const auto value : {0.1, 0.5, 1.0, 2.0, 5.0, 10.0}) {
        speed->addItem(QStringLiteral("%1×").arg(value), value);
    }
    speed->setCurrentIndex(2);
    controls->addWidget(openButton);
    controls->addWidget(closeButton_);
    controls->addSpacing(12);
    controls->addWidget(playButton_);
    controls->addWidget(new QLabel(tr("速度"), this));
    controls->addWidget(speed);
    controls->addStretch();
    root->addLayout(controls);

    pathLabel_ = new QLabel(tr("尚未打开 Session"), this);
    pathLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pathLabel_->setStyleSheet(QStringLiteral("color: #8794a6;"));
    root->addWidget(pathLabel_);

    auto* status = new QHBoxLayout;
    stateLabel_ = new QLabel(tr("已关闭"), this);
    timeLabel_ = new QLabel(QStringLiteral("00:00.000 / 00:00.000"), this);
    countLabel_ = new QLabel(tr("记录 0 / 0"), this);
    status->addWidget(stateLabel_);
    status->addStretch();
    status->addWidget(countLabel_);
    status->addWidget(timeLabel_);
    root->addLayout(status);

    timeline_ = new QSlider(Qt::Horizontal, this);
    timeline_->setRange(0, 10'000);
    timeline_->setEnabled(false);
    root->addWidget(timeline_);

    auto* help = new QLabel(
        tr("回放数据会进入与实时串口相同的终端、协议解析和曲线链路。打开后默认暂停，"
           "拖动时间轴可跳转；Session 中保存的初始协议会自动加载。"),
        this);
    help->setWordWrap(true);
    help->setStyleSheet(QStringLiteral("color: #8794a6; padding-top: 12px;"));
    root->addWidget(help);
    root->addStretch();

    connect(openButton, &QPushButton::clicked, this, [this] {
        const auto directory = QFileDialog::getExistingDirectory(
            this, tr("选择 Lab Debugger Session 目录"));
        if (!directory.isEmpty()) {
            emit openSessionRequested(directory);
        }
    });
    connect(closeButton_, &QPushButton::clicked, this, &ReplayWidget::closeReplayRequested);
    connect(playButton_, &QPushButton::clicked, this, [this] {
        if (paused_) {
            emit resumeRequested();
        } else {
            emit pauseRequested();
        }
    });
    connect(speed, &QComboBox::currentIndexChanged, this, [this, speed](int index) {
        emit speedChanged(speed->itemData(index).toDouble());
    });
    connect(timeline_, &QSlider::sliderReleased, this, [this] {
        emit seekRequested(static_cast<double>(timeline_->value()) / timeline_->maximum());
    });
}

void ReplayWidget::setOpened(const QString& directory, bool recoveredTruncatedTail) {
    pathLabel_->setText(tr("Session：%1").arg(directory));
    pathLabel_->setStyleSheet(recoveredTruncatedTail
                                  ? QStringLiteral("color: #f2cc60;")
                                  : QStringLiteral("color: #5fd19a;"));
    if (recoveredTruncatedTail) {
        pathLabel_->setToolTip(tr("文件尾部不完整，已安全恢复此前的全部完整记录"));
    } else {
        pathLabel_->setToolTip({});
    }
}

void ReplayWidget::showOpenError(const QString& message) {
    QMessageBox::warning(this, tr("无法打开回放"), message);
}

void ReplayWidget::setStatus(bool open,
                             bool paused,
                             bool atEnd,
                             double,
                             quint64 position,
                             quint64 recordCount,
                             qint64 firstTimestamp,
                             qint64 lastTimestamp,
                             qint64 currentTimestamp) {
    paused_ = paused;
    playButton_->setEnabled(open && !atEnd);
    closeButton_->setEnabled(open);
    timeline_->setEnabled(open && recordCount > 0);
    playButton_->setText(paused ? tr("播放") : tr("暂停"));
    stateLabel_->setText(!open ? tr("已关闭")
                               : atEnd ? tr("已到末尾")
                                       : paused ? tr("已暂停") : tr("正在回放"));
    stateLabel_->setStyleSheet(open && !paused ? QStringLiteral("color: #5fd19a;")
                                               : QStringLiteral("color: #8794a6;"));
    countLabel_->setText(tr("记录 %1 / %2").arg(position).arg(recordCount));
    const auto duration = std::max<qint64>(0, lastTimestamp - firstTimestamp);
    const auto elapsed = std::clamp<qint64>(currentTimestamp - firstTimestamp, 0, duration);
    timeLabel_->setText(tr("%1 / %2").arg(formatDuration(elapsed), formatDuration(duration)));
    if (!timeline_->isSliderDown()) {
        const auto fraction = duration > 0 ? static_cast<double>(elapsed) / duration : 0.0;
        timeline_->setValue(static_cast<int>(fraction * timeline_->maximum()));
    }
}

}  // namespace lab::ui

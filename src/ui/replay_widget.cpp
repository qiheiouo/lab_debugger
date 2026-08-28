#include "ui/replay_widget.hpp"

#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSlider>
#include <QStandardPaths>
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
    importButton_ = new QPushButton(tr("导入 .db3"), this);
    importDirectoryButton_ = new QPushButton(tr("导入 bag 目录"), this);
    cancelImportButton_ = new QPushButton(tr("取消导入"), this);
    cancelImportButton_->setEnabled(false);
    closeButton_ = new QPushButton(tr("关闭回放"), this);
    playButton_ = new QPushButton(tr("播放"), this);
    auto* speed = new QComboBox(this);
    for (const auto value : {0.1, 0.5, 1.0, 2.0, 5.0, 10.0}) {
        speed->addItem(QStringLiteral("%1×").arg(value), value);
    }
    speed->setCurrentIndex(2);
    controls->addWidget(openButton);
    controls->addWidget(importButton_);
    controls->addWidget(importDirectoryButton_);
    controls->addWidget(cancelImportButton_);
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

    importLabel_ = new QLabel(tr("可导入 rosbag2 SQLite（.db3）并生成标准 Session"), this);
    importLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    importLabel_->setStyleSheet(QStringLiteral("color: #8794a6;"));
    root->addWidget(importLabel_);

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
        tr("普通 Session 会进入终端、协议解析和曲线链路；rosbag2 的 CDR 二进制仅进入"
           "原始终端与回放，不会误送进 CSV 解析。打开后默认暂停，拖动时间轴可跳转。"),
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
    connect(importButton_, &QPushButton::clicked, this, [this] {
        chooseRosbagSource(false);
    });
    connect(importDirectoryButton_, &QPushButton::clicked, this, [this] {
        chooseRosbagSource(true);
    });
    connect(cancelImportButton_,
            &QPushButton::clicked,
            this,
            &ReplayWidget::cancelRosbagImportRequested);
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

void ReplayWidget::chooseRosbagSource(bool directory) {
    const auto documents =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const auto source = directory
                            ? QFileDialog::getExistingDirectory(
                                  this, tr("选择 rosbag2 bag 目录"), documents)
                            : QFileDialog::getOpenFileName(
                                  this,
                                  tr("选择 rosbag2 SQLite 文件"),
                                  documents,
                                  tr("rosbag2 SQLite (*.db3)"));
    if (source.isEmpty()) {
        return;
    }
    const auto parent = QFileDialog::getExistingDirectory(
        this, tr("选择导入后 Session 的保存位置"), documents);
    if (parent.isEmpty()) {
        return;
    }
    const QFileInfo sourceInfo(source);
    auto stem = sourceInfo.isDir() ? sourceInfo.fileName() : sourceInfo.completeBaseName();
    stem.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]+")),
                 QStringLiteral("_"));
    if (stem.isEmpty()) {
        stem = QStringLiteral("rosbag2");
    }
    const auto name = QStringLiteral("%1_import_%2")
                          .arg(stem,
                               QDateTime::currentDateTime().toString(
                                   QStringLiteral("yyyyMMdd_HHmmss")));
    emit importRosbagRequested(source, QDir(parent).filePath(name));
}

void ReplayWidget::setOpened(const QString& directory,
                             bool recoveredTruncatedTail,
                             bool rawOnly) {
    pathLabel_->setText(rawOnly ? tr("Session（rosbag2 原始 CDR）：%1").arg(directory)
                                : tr("Session：%1").arg(directory));
    pathLabel_->setStyleSheet(recoveredTruncatedTail
                                  ? QStringLiteral("color: #f2cc60;")
                                  : QStringLiteral("color: #5fd19a;"));
    if (recoveredTruncatedTail) {
        pathLabel_->setToolTip(tr("文件尾部不完整，已安全恢复此前的全部完整记录"));
    } else {
        pathLabel_->setToolTip({});
    }
}

void ReplayWidget::setImportStarted(const QString& source, const QString& destination) {
    importButton_->setEnabled(false);
    importDirectoryButton_->setEnabled(false);
    cancelImportButton_->setEnabled(true);
    importLabel_->setText(tr("正在导入：%1\n保存到：%2").arg(source, destination));
    importLabel_->setStyleSheet(QStringLiteral("color: #f2cc60;"));
}

void ReplayWidget::setImportProgress(quint64 importedMessages, quint64 totalMessages) {
    importLabel_->setText(totalMessages > 0
                              ? tr("正在导入 rosbag2：%1 / %2 条消息")
                                    .arg(importedMessages)
                                    .arg(totalMessages)
                              : tr("正在检查 rosbag2 数据库…"));
}

void ReplayWidget::setImportFinished(bool success,
                                     const QString& directory,
                                     const QString& message,
                                     quint64,
                                     quint64) {
    importButton_->setEnabled(true);
    importDirectoryButton_->setEnabled(true);
    cancelImportButton_->setEnabled(false);
    importLabel_->setText(success ? tr("%1\nSession：%2").arg(message, directory) : message);
    importLabel_->setStyleSheet(success ? QStringLiteral("color: #5fd19a;")
                                        : QStringLiteral("color: #ff6b6b;"));
    if (!success && !message.contains(tr("已取消"))) {
        QMessageBox::warning(this, tr("rosbag2 导入失败"), message);
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

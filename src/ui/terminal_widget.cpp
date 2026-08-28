#include "ui/terminal_widget.hpp"

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTextStream>
#include <QVBoxLayout>

namespace lab::ui {

TerminalWidget::TerminalWidget(QWidget* parent) : QWidget(parent) {
    mode_ = new QComboBox(this);
    mode_->addItem(QStringLiteral("HEX"));
    mode_->addItem(QStringLiteral("ASCII"));
    autoScroll_ = new QCheckBox(tr("自动滚动"), this);
    autoScroll_->setChecked(true);
    paused_ = new QCheckBox(tr("暂停显示"), this);
    auto* clearButton = new QPushButton(tr("清空"), this);
    auto* copyButton = new QPushButton(tr("复制全部"), this);
    auto* saveButton = new QPushButton(tr("保存文本"), this);

    auto* controls = new QHBoxLayout;
    controls->addWidget(new QLabel(tr("显示"), this));
    controls->addWidget(mode_);
    controls->addSpacing(12);
    controls->addWidget(autoScroll_);
    controls->addWidget(paused_);
    controls->addStretch(1);
    controls->addWidget(clearButton);
    controls->addWidget(copyButton);
    controls->addWidget(saveButton);

    output_ = new QPlainTextEdit(this);
    output_->setReadOnly(true);
    output_->setMaximumBlockCount(50'000);
    output_->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont terminalFont(QStringLiteral("Cascadia Mono"));
    terminalFont.setStyleHint(QFont::Monospace);
    output_->setFont(terminalFont);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->addLayout(controls);
    layout->addWidget(output_, 1);

    connect(clearButton, &QPushButton::clicked, output_, &QPlainTextEdit::clear);
    connect(copyButton, &QPushButton::clicked, this, [this] {
        QGuiApplication::clipboard()->setText(output_->toPlainText());
    });
    connect(saveButton, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getSaveFileName(
            this, tr("保存终端文本"), QStringLiteral("terminal.txt"), tr("文本文件 (*.txt);;所有文件 (*)"));
        if (path.isEmpty()) {
            return;
        }
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream(&file) << output_->toPlainText();
        }
    });
}

void TerminalWidget::appendChunk(
    const QByteArray& bytes,
    bool transmitted,
    qint64 timestampNs,
    const QString& sourceId) {
    if (paused_->isChecked()) {
        return;
    }
    const auto milliseconds = timestampNs / 1'000'000;
    const auto time = QDateTime::fromMSecsSinceEpoch(milliseconds).toString(QStringLiteral("HH:mm:ss.zzz"));
    const auto direction = transmitted ? QStringLiteral("TX") : QStringLiteral("RX");
    output_->appendPlainText(
        QStringLiteral("[%1] %2  [%3]  %4")
            .arg(time, direction, sourceId, formatPayload(bytes)));
    if (autoScroll_->isChecked()) {
        output_->verticalScrollBar()->setValue(output_->verticalScrollBar()->maximum());
    }
}

QString TerminalWidget::formatPayload(const QByteArray& bytes) const {
    if (mode_->currentText() == QStringLiteral("HEX")) {
        return QString::fromLatin1(bytes.toHex(' ').toUpper());
    }
    QString result;
    result.reserve(bytes.size());
    for (const auto byte : bytes) {
        const auto value = static_cast<unsigned char>(byte);
        if (value == '\r') {
            result += QStringLiteral("\\r");
        } else if (value == '\n') {
            result += QStringLiteral("\\n");
        } else if (value == '\t') {
            result += QStringLiteral("\\t");
        } else if (value >= 32 && value <= 126) {
            result += QChar(value);
        } else {
            result += QChar(0x00B7);
        }
    }
    return result;
}

}  // namespace lab::ui

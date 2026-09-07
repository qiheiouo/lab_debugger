#include "ui/send_panel.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTimer>

namespace lab::ui {

SendPanel::SendPanel(QWidget* parent) : QWidget(parent) {
    target_ = new QComboBox(this);
    target_->addItem(tr("串口"), 0);
    target_->addItem(tr("网络"), 1);

    mode_ = new QComboBox(this);
    mode_->addItems({QStringLiteral("ASCII"), QStringLiteral("HEX")});

    input_ = new QComboBox(this);
    input_->setEditable(true);
    input_->setInsertPolicy(QComboBox::NoInsert);
    input_->setMinimumWidth(300);
    input_->lineEdit()->setPlaceholderText(tr("输入文本，或 HEX：AA 55 01 04"));

    lineEnding_ = new QComboBox(this);
    lineEnding_->addItem(tr("无结尾"), QByteArray());
    lineEnding_->addItem(QStringLiteral("\\r"), QByteArray("\r"));
    lineEnding_->addItem(QStringLiteral("\\n"), QByteArray("\n"));
    lineEnding_->addItem(QStringLiteral("\\r\\n"), QByteArray("\r\n"));

    auto* sendButton = new QPushButton(tr("发送"), this);
    auto* favoriteButton = new QPushButton(tr("收藏"), this);
    favorites_ = new QComboBox(this);
    favorites_->setMinimumWidth(130);
    favorites_->addItem(tr("收藏命令…"));

    periodic_ = new QCheckBox(tr("循环"), this);
    periodMs_ = new QSpinBox(this);
    periodMs_->setRange(10, 3'600'000);
    periodMs_->setValue(100);
    periodMs_->setSuffix(QStringLiteral(" ms"));
    validation_ = new QLabel(this);
    validation_->setStyleSheet(QStringLiteral("color: #ff6b6b;"));

    timer_ = new QTimer(this);
    timer_->setTimerType(Qt::PreciseTimer);

    auto* firstRow = new QHBoxLayout;
    firstRow->addWidget(new QLabel(tr("发送到"), this));
    firstRow->addWidget(target_);
    firstRow->addWidget(mode_);
    firstRow->addWidget(input_, 1);
    firstRow->addWidget(lineEnding_);
    firstRow->addWidget(sendButton);

    auto* secondRow = new QHBoxLayout;
    secondRow->addWidget(favorites_);
    secondRow->addWidget(favoriteButton);
    secondRow->addSpacing(16);
    secondRow->addWidget(periodic_);
    secondRow->addWidget(periodMs_);
    secondRow->addWidget(validation_, 1);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 8);
    layout->addLayout(firstRow);
    layout->addLayout(secondRow);

    QSettings settings;
    setTarget(settings.value(QStringLiteral("send/target"), 0).toInt());
    input_->addItems(settings.value(QStringLiteral("send/history")).toStringList());
    for (const auto& favorite : settings.value(QStringLiteral("send/favorites")).toStringList()) {
        favorites_->addItem(favorite);
    }

    connect(sendButton, &QPushButton::clicked, this, &SendPanel::sendNow);
    connect(input_->lineEdit(), &QLineEdit::returnPressed, this, &SendPanel::sendNow);
    connect(favoriteButton, &QPushButton::clicked, this, &SendPanel::addFavorite);
    connect(periodic_, &QCheckBox::toggled, this, &SendPanel::updateTimer);
    connect(periodMs_, &QSpinBox::valueChanged, this, &SendPanel::updateTimer);
    connect(timer_, &QTimer::timeout, this, &SendPanel::sendNow);
    connect(target_, &QComboBox::currentIndexChanged, this, [this](int) {
        const auto selected = target();
        QSettings().setValue(QStringLiteral("send/target"), selected);
        emit targetChanged(selected);
    });
    connect(favorites_, &QComboBox::activated, this, [this](int index) {
        if (index > 0) {
            input_->setCurrentText(favorites_->itemText(index));
        }
    });
}

int SendPanel::target() const {
    return target_->currentData().toInt();
}

void SendPanel::setTarget(int target) {
    const auto index = target_->findData(target);
    if (index >= 0) {
        target_->setCurrentIndex(index);
    }
}

void SendPanel::sendNow() {
    const auto bytes = payload();
    if (!bytes) {
        validation_->setText(tr("HEX 格式无效"));
        return;
    }
    if (bytes->isEmpty()) {
        validation_->setText(tr("请输入发送内容"));
        return;
    }
    validation_->clear();
    rememberHistory(input_->currentText());
    emit sendRequested(*bytes);
}

void SendPanel::addFavorite() {
    const auto text = input_->currentText().trimmed();
    if (text.isEmpty() || favorites_->findText(text) >= 0) {
        return;
    }
    favorites_->addItem(text);
    saveFavorites();
}

void SendPanel::updateTimer() {
    timer_->setInterval(periodMs_->value());
    if (periodic_->isChecked()) {
        timer_->start();
    } else {
        timer_->stop();
    }
}

std::optional<QByteArray> SendPanel::payload() const {
    QByteArray result;
    if (mode_->currentText() == QStringLiteral("HEX")) {
        auto compact = input_->currentText();
        compact.remove(' ');
        compact.remove('\t');
        compact.remove('-');
        if (compact.size() % 2 != 0) {
            return std::nullopt;
        }
        for (const auto character : compact) {
            if (!character.isDigit() &&
                !(character.toLower() >= QChar('a') && character.toLower() <= QChar('f'))) {
                return std::nullopt;
            }
        }
        result = QByteArray::fromHex(compact.toLatin1());
    } else {
        result = input_->currentText().toUtf8();
    }
    result += lineEnding_->currentData().toByteArray();
    return result;
}

void SendPanel::rememberHistory(const QString& text) {
    if (text.isEmpty()) {
        return;
    }
    const auto existing = input_->findText(text);
    if (existing >= 0) {
        input_->removeItem(existing);
    }
    input_->insertItem(0, text);
    while (input_->count() > 50) {
        input_->removeItem(input_->count() - 1);
    }
    QStringList history;
    for (int index = 0; index < input_->count(); ++index) {
        history.push_back(input_->itemText(index));
    }
    QSettings().setValue(QStringLiteral("send/history"), history);
}

void SendPanel::saveFavorites() const {
    QStringList favorites;
    for (int index = 1; index < favorites_->count(); ++index) {
        favorites.push_back(favorites_->itemText(index));
    }
    QSettings().setValue(QStringLiteral("send/favorites"), favorites);
}

}  // namespace lab::ui

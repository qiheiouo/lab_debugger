#include "ui/protocol_widget.hpp"

#include <QAbstractItemView>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QFileDialog>
#include <QFontDatabase>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QVariantMap>

namespace lab::ui {
namespace {

QString eventLabel(const QString& kind) {
    if (kind == QStringLiteral("frame")) {
        return QObject::tr("有效帧");
    }
    if (kind == QStringLiteral("garbage")) {
        return QObject::tr("跳过字节");
    }
    if (kind == QStringLiteral("checksum_error")) {
        return QObject::tr("校验失败");
    }
    if (kind == QStringLiteral("length_error")) {
        return QObject::tr("长度异常");
    }
    return QObject::tr("解析失败");
}

}  // namespace

ProtocolWidget::ProtocolWidget(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    auto* targetRow = new QHBoxLayout;
    sourceSelector_ = new QComboBox(this);
    sourceSelector_->setObjectName(QStringLiteral("parserSourceSelector"));
    sourceSelector_->addItem(tr("全部本地字节流（默认）"), QString{});
    resetSourceButton_ = new QPushButton(tr("恢复默认"), this);
    resetSourceButton_->setObjectName(QStringLiteral("resetSourceParserButton"));
    resetSourceButton_->setEnabled(false);
    targetRow->addWidget(new QLabel(tr("配置目标"), this));
    targetRow->addWidget(sourceSelector_, 1);
    targetRow->addWidget(resetSourceButton_);
    root->addLayout(targetRow);

    auto* toolbar = new QHBoxLayout;
    csvFields_ = new QLineEdit(QStringLiteral("speed,current,voltage"), this);
    csvFields_->setObjectName(QStringLiteral("sourceCsvFields"));
    csvFields_->setPlaceholderText(tr("例如：speed,current,voltage"));
    auto* applyCsvButton = new QPushButton(tr("应用 CSV 字段"), this);
    applyCsvButton->setObjectName(QStringLiteral("applySourceCsvButton"));
    auto* loadButton = new QPushButton(tr("加载 JSON 协议"), this);
    auto* disableButton = new QPushButton(tr("停用协议"), this);
    statusLabel_ = new QLabel(tr("尚未加载协议"), this);
    statusLabel_->setStyleSheet(QStringLiteral("color: #7c8799;"));
    toolbar->addWidget(new QLabel(tr("CSV 字段"), this));
    toolbar->addWidget(csvFields_, 1);
    toolbar->addWidget(applyCsvButton);
    toolbar->addWidget(loadButton);
    toolbar->addWidget(disableButton);
    toolbar->addSpacing(12);
    toolbar->addWidget(statusLabel_, 1);
    root->addLayout(toolbar);

    auto* summary = new QHBoxLayout;
    decodedLabel_ = new QLabel(tr("有效帧 0"), this);
    discardedLabel_ = new QLabel(tr("跳过字节 0"), this);
    checksumLabel_ = new QLabel(tr("校验错误 0"), this);
    lengthLabel_ = new QLabel(tr("长度错误 0"), this);
    decodeLabel_ = new QLabel(tr("解析错误 0"), this);
    fieldsLabel_ = new QLabel(tr("曲线字段：—"), this);
    for (auto* label : {decodedLabel_, discardedLabel_, checksumLabel_, lengthLabel_, decodeLabel_}) {
        label->setStyleSheet(
            QStringLiteral("padding: 4px 8px; background: #192130; border-radius: 4px;"));
        summary->addWidget(label);
    }
    summary->addStretch();
    root->addLayout(summary);
    root->addWidget(fieldsLabel_);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    packets_ = new QTableWidget(0, 4, splitter);
    packets_->setHorizontalHeaderLabels({tr("时间"), tr("结果"), tr("字节数"), tr("说明")});
    packets_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    packets_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    packets_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    packets_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    packets_->setSelectionBehavior(QAbstractItemView::SelectRows);
    packets_->setEditTriggers(QAbstractItemView::NoEditTriggers);

    auto* detail = new QWidget(splitter);
    auto* detailLayout = new QVBoxLayout(detail);
    detailLayout->setContentsMargins(0, 0, 0, 0);
    fields_ = new QTableWidget(0, 4, detail);
    fields_->setHorizontalHeaderLabels({tr("字段"), tr("值"), tr("单位"), tr("类型")});
    fields_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    fields_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    fields_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    fields_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    fields_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    rawHex_ = new QPlainTextEdit(detail);
    rawHex_->setReadOnly(true);
    rawHex_->setPlaceholderText(tr("最近一帧的原始十六进制数据"));
    rawHex_->setMaximumBlockCount(32);
    rawHex_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    detailLayout->addWidget(new QLabel(tr("最近有效帧字段"), detail));
    detailLayout->addWidget(fields_, 3);
    detailLayout->addWidget(new QLabel(tr("原始帧"), detail));
    detailLayout->addWidget(rawHex_, 1);

    splitter->addWidget(packets_);
    splitter->addWidget(detail);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    root->addWidget(splitter, 1);

    connect(loadButton, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getOpenFileName(
            this, tr("选择协议定义"), {}, tr("JSON 协议 (*.json);;所有文件 (*.*)"));
        if (!path.isEmpty()) {
            emit protocolConfigurationRequested(currentSourceId(), path);
        }
    });
    const auto applyCsv = [this] {
        const auto fields = csvFields_->text().split(
            QLatin1Char(','), Qt::SkipEmptyParts);
        emit csvConfigurationRequested(currentSourceId(), fields);
    };
    connect(applyCsvButton, &QPushButton::clicked, this, applyCsv);
    connect(csvFields_, &QLineEdit::returnPressed, this, applyCsv);
    connect(disableButton, &QPushButton::clicked, this, [this] {
        emit disableProtocolConfigurationRequested(currentSourceId());
    });
    connect(resetSourceButton_, &QPushButton::clicked, this, [this] {
        const auto sourceId = currentSourceId();
        if (!sourceId.isEmpty()) emit resetSourceConfigurationRequested(sourceId);
    });
    connect(sourceSelector_, &QComboBox::currentIndexChanged,
            this, [this] { showSelectedConfiguration(); });
    configurationFields_.insert(QString{}, {QStringLiteral("speed"),
                                            QStringLiteral("current"),
                                            QStringLiteral("voltage")});
    configurationSummaries_.insert(QString{}, tr("默认：CSV 解析"));
}

void ProtocolWidget::setProtocolLoaded(const QString& name, const QStringList& numericFields) {
    showParserConfigured({}, QStringLiteral("protocol"), name, numericFields);
    packets_->setRowCount(0);
    fields_->setRowCount(0);
    rawHex_->clear();
}

void ProtocolWidget::setProtocolCleared() {
    showParserConfigured({}, QStringLiteral("csv"), {},
                         configurationFields_.value(QString{}));
    setStatistics(0, 0, 0, 0, 0);
}

void ProtocolWidget::showLoadErrors(const QStringList& issues) {
    showParserErrors({}, issues);
}

QString ProtocolWidget::currentSourceId() const {
    return sourceSelector_->currentData().toString();
}

void ProtocolWidget::setParserSources(const QVariantList& sources) {
    const auto previous = currentSourceId();
    sourceSelector_->blockSignals(true);
    sourceSelector_->clear();
    sourceSelector_->addItem(tr("全部本地字节流（默认）"), QString{});
    for (const auto& value : sources) {
        const auto source = value.toMap();
        const auto id = source.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) continue;
        auto label = source.value(QStringLiteral("label"), id).toString();
        if (source.value(QStringLiteral("overridden")).toBool()) {
            label += source.value(QStringLiteral("mode")).toString() ==
                             QStringLiteral("protocol")
                         ? tr("  [独立协议]")
                         : tr("  [独立 CSV]");
        }
        sourceSelector_->addItem(label, id);
    }
    const auto restoredIndex = sourceSelector_->findData(previous);
    sourceSelector_->setCurrentIndex(restoredIndex < 0 ? 0 : restoredIndex);
    sourceSelector_->blockSignals(false);
    showSelectedConfiguration();
}

void ProtocolWidget::showParserConfigured(QString sourceId,
                                           QString mode,
                                           QString name,
                                           QStringList fields) {
    configurationFields_.insert(sourceId, fields);
    const auto target = sourceId.isEmpty() ? tr("默认") : sourceId;
    const auto summary = mode == QStringLiteral("protocol")
                             ? tr("%1：协议 %2").arg(target, name)
                             : mode == QStringLiteral("default")
                                   ? tr("%1：继承默认解析配置").arg(target)
                                   : tr("%1：CSV 解析").arg(target);
    configurationSummaries_.insert(sourceId, summary);
    if (sourceId == currentSourceId()) showSelectedConfiguration();
}

void ProtocolWidget::showParserErrors(QString sourceId,
                                      const QStringList& issues) {
    if (sourceId == currentSourceId()) {
        statusLabel_->setText(tr("解析配置失败：%1").arg(issues.value(0)));
        statusLabel_->setStyleSheet(QStringLiteral("color: #ff7875;"));
    }
    QMessageBox::warning(this, tr("解析配置无效"), issues.join(QLatin1Char('\n')));
}

void ProtocolWidget::showSelectedConfiguration() {
    const auto sourceId = currentSourceId();
    resetSourceButton_->setEnabled(!sourceId.isEmpty());
    const auto fields = configurationFields_.value(
        sourceId, configurationFields_.value(QString{}));
    csvFields_->setText(fields.join(QLatin1Char(',')));
    statusLabel_->setText(configurationSummaries_.value(
        sourceId,
        sourceId.isEmpty() ? tr("默认：CSV 解析")
                           : tr("%1：继承默认解析配置").arg(sourceId)));
    statusLabel_->setStyleSheet(QStringLiteral("color: #5fd19a;"));
    fieldsLabel_->setText(tr("字段：%1").arg(
        fields.isEmpty() ? tr("无数值字段")
                         : fields.join(QStringLiteral(", "))));
}

void ProtocolWidget::appendEvents(const QVariantList& events) {
    constexpr int maximumRows = 2000;
    for (const auto& value : events) {
        const auto event = value.toMap();
        const auto kind = event.value(QStringLiteral("kind")).toString();
        const auto raw = event.value(QStringLiteral("raw")).toByteArray();
        while (packets_->rowCount() >= maximumRows) {
            packets_->removeRow(0);
        }
        const auto row = packets_->rowCount();
        packets_->insertRow(row);
        const auto timestampMs = event.value(QStringLiteral("timestampNs")).toLongLong() / 1'000'000;
        packets_->setItem(
            row, 0,
            new QTableWidgetItem(QDateTime::fromMSecsSinceEpoch(timestampMs)
                                     .toString(QStringLiteral("HH:mm:ss.zzz"))));
        auto* status = new QTableWidgetItem(eventLabel(kind));
        status->setForeground(kind == QStringLiteral("frame")
                                  ? QColor(QStringLiteral("#5fd19a"))
                                  : QColor(QStringLiteral("#ff7875")));
        packets_->setItem(row, 1, status);
        packets_->setItem(row, 2, new QTableWidgetItem(tr("%1 B").arg(raw.size())));
        packets_->setItem(
            row, 3, new QTableWidgetItem(event.value(QStringLiteral("message")).toString()));

        if (kind == QStringLiteral("frame")) {
            const auto fieldList = event.value(QStringLiteral("fields")).toList();
            fields_->setRowCount(static_cast<int>(fieldList.size()));
            for (qsizetype index = 0; index < fieldList.size(); ++index) {
                const auto field = fieldList.at(index).toMap();
                fields_->setItem(static_cast<int>(index), 0,
                                 new QTableWidgetItem(field.value("name").toString()));
                fields_->setItem(static_cast<int>(index), 1,
                                 new QTableWidgetItem(field.value("value").toString()));
                fields_->setItem(static_cast<int>(index), 2,
                                 new QTableWidgetItem(field.value("unit").toString()));
                fields_->setItem(static_cast<int>(index), 3,
                                 new QTableWidgetItem(field.value("type").toString()));
            }
            rawHex_->setPlainText(QString::fromLatin1(raw.toHex(' ').toUpper()));
        }
    }
    packets_->scrollToBottom();
}

void ProtocolWidget::setStatistics(quint64 decoded,
                                   quint64 discardedBytes,
                                   quint64 checksumErrors,
                                   quint64 lengthErrors,
                                   quint64 decodeErrors) {
    decodedLabel_->setText(tr("有效帧 %1").arg(decoded));
    discardedLabel_->setText(tr("跳过字节 %1").arg(discardedBytes));
    checksumLabel_->setText(tr("校验错误 %1").arg(checksumErrors));
    lengthLabel_->setText(tr("长度错误 %1").arg(lengthErrors));
    decodeLabel_->setText(tr("解析错误 %1").arg(decodeErrors));
}

}  // namespace lab::ui

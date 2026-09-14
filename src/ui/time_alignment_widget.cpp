#include "ui/time_alignment_widget.hpp"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>

namespace lab::ui {
namespace {

QString modeLabel(const QString& mode) {
    if (mode == QStringLiteral("receive")) return QObject::tr("电脑接收时间");
    if (mode == QStringLiteral("manual")) return QObject::tr("源时间 + 手动偏移");
    if (mode == QStringLiteral("remote_clock")) return QObject::tr("ROS Agent 自动校正");
    return QObject::tr("原始源时间");
}

QString milliseconds(const QVariant& nanoseconds) {
    if (!nanoseconds.isValid() || nanoseconds.isNull()) return QStringLiteral("—");
    return QString::number(nanoseconds.toLongLong() / 1'000'000.0, 'f', 3);
}

}  // namespace

TimeAlignmentWidget::TimeAlignmentWidget(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("timeAlignmentWidget"));

    auto* explanation = new QLabel(
        tr("默认使用设备或 ROS Header 的源时间。可按完整 sourceId 改用电脑接收时间、"
           "手动偏移，或让 ROS Agent 的 Ping/Pong 结果自动校正。对 Agent 身份设置的"
           "规则会覆盖它的全部 Topic，Topic 自己的规则优先。原始源时间与接收时间始终保留。"),
        this);
    explanation->setWordWrap(true);
    explanation->setObjectName(QStringLiteral("secondaryText"));

    rules_ = new QTableWidget(0, 3, this);
    rules_->setObjectName(QStringLiteral("timeAlignmentRules"));
    rules_->setHorizontalHeaderLabels(
        {tr("来源 sourceId"), tr("时间策略"), tr("手动偏移 (ms)")});
    rules_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    rules_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    rules_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    rules_->verticalHeader()->hide();
    rules_->setSelectionBehavior(QAbstractItemView::SelectRows);

    auto* addButton = new QPushButton(tr("添加策略"), this);
    removeButton_ = new QPushButton(tr("删除选中"), this);
    auto* applyButton = new QPushButton(tr("应用策略"), this);
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(addButton);
    buttons->addWidget(removeButton_);
    buttons->addStretch(1);
    buttons->addWidget(applyButton);

    status_ = new QLabel(tr("未配置时使用原始源时间"), this);
    status_->setWordWrap(true);
    status_->setObjectName(QStringLiteral("timeAlignmentConfigurationStatus"));

    auto* ruleBox = new QGroupBox(tr("逐来源时间策略"), this);
    auto* ruleLayout = new QVBoxLayout(ruleBox);
    ruleLayout->addWidget(rules_);
    ruleLayout->addLayout(buttons);
    ruleLayout->addWidget(status_);

    statuses_ = new QTableWidget(0, 7, this);
    statuses_->setObjectName(QStringLiteral("timeAlignmentStatuses"));
    statuses_->setHorizontalHeaderLabels(
        {tr("来源"), tr("当前策略"), tr("修正 ms"), tr("延迟 ms"),
         tr("不确定度 ms"), tr("样本/回退"), tr("乱序")});
    statuses_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < statuses_->columnCount(); ++column) {
        statuses_->horizontalHeader()->setSectionResizeMode(
            column, QHeaderView::ResizeToContents);
    }
    statuses_->verticalHeader()->hide();
    statuses_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    statuses_->setSelectionBehavior(QAbstractItemView::SelectRows);

    auto* statusBox = new QGroupBox(tr("实时同步质量"), this);
    auto* statusLayout = new QVBoxLayout(statusBox);
    statusLayout->addWidget(new QLabel(
        tr("延迟 = 电脑接收时间 − 对齐后时间；回退表示自动校正尚未取得有效样本，"
           "乱序表示该来源的对齐时间向后跳变。"), statusBox));
    statusLayout->addWidget(statuses_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->addWidget(explanation);
    layout->addWidget(ruleBox, 1);
    layout->addWidget(statusBox, 1);

    connect(addButton, &QPushButton::clicked, this, &TimeAlignmentWidget::addRule);
    connect(removeButton_, &QPushButton::clicked,
            this, &TimeAlignmentWidget::removeSelectedRules);
    connect(applyButton, &QPushButton::clicked, this, [this] {
        emit applyRequested(definitions());
    });
}

QVariantList TimeAlignmentWidget::definitions() const {
    QVariantList result;
    result.reserve(rules_->rowCount());
    for (int row = 0; row < rules_->rowCount(); ++row) {
        const auto* source = rules_->item(row, 0);
        const auto* mode = qobject_cast<QComboBox*>(rules_->cellWidget(row, 1));
        const auto* offset = qobject_cast<QDoubleSpinBox*>(rules_->cellWidget(row, 2));
        if (!source || !mode || !offset) continue;
        QVariantMap definition;
        definition.insert(QStringLiteral("source_id"), source->text().trimmed());
        definition.insert(QStringLiteral("mode"), mode->currentData().toString());
        const auto offsetNs = static_cast<qlonglong>(
            std::llround(offset->value() * 1'000'000.0));
        if (mode->currentData().toString() == QStringLiteral("manual")) {
            definition.insert(QStringLiteral("offset_ns"), offsetNs);
        }
        result.push_back(definition);
    }
    return result;
}

void TimeAlignmentWidget::setDefinitions(const QVariantList& definitions) {
    rules_->setRowCount(0);
    for (const auto& value : definitions) appendRule(value.toMap());
}

void TimeAlignmentWidget::setStatuses(const QVariantList& statuses) {
    statuses_->setRowCount(statuses.size());
    for (int row = 0; row < statuses.size(); ++row) {
        const auto item = statuses[row].toMap();
        const auto samples = item.value(QStringLiteral("sample_count")).toULongLong();
        const auto fallbacks = item.value(QStringLiteral("fallback_count")).toULongLong();
        const QStringList values{
            item.value(QStringLiteral("source_id")).toString(),
            modeLabel(item.value(QStringLiteral("mode")).toString()),
            milliseconds(item.value(QStringLiteral("applied_offset_ns"))),
            milliseconds(item.value(QStringLiteral("latency_ns"))),
            milliseconds(item.value(QStringLiteral("uncertainty_ns"))),
            QStringLiteral("%1 / %2").arg(samples).arg(fallbacks),
            QString::number(item.value(QStringLiteral("out_of_order_count")).toULongLong())};
        for (int column = 0; column < statuses_->columnCount(); ++column) {
            statuses_->setItem(row, column, new QTableWidgetItem(values[column]));
        }
    }
}

void TimeAlignmentWidget::showConfigurationResult(
    bool success,
    const QStringList& messages) {
    status_->setText(messages.join(QStringLiteral("；")));
    status_->setStyleSheet(success ? QStringLiteral("color: #5ad49b;")
                                   : QStringLiteral("color: #ff7b72;"));
}

void TimeAlignmentWidget::addRule() {
    appendRule();
}

void TimeAlignmentWidget::removeSelectedRules() {
    auto rows = rules_->selectionModel()->selectedRows();
    std::ranges::sort(rows, std::greater{}, &QModelIndex::row);
    for (const auto& index : rows) rules_->removeRow(index.row());
}

void TimeAlignmentWidget::appendRule(const QVariantMap& definition) {
    const auto row = rules_->rowCount();
    rules_->insertRow(row);
    rules_->setItem(
        row, 0,
        new QTableWidgetItem(definition.value(QStringLiteral("source_id")).toString()));

    auto* mode = new QComboBox(rules_);
    mode->addItem(tr("原始源时间"), QStringLiteral("source"));
    mode->addItem(tr("电脑接收时间"), QStringLiteral("receive"));
    mode->addItem(tr("源时间 + 手动偏移"), QStringLiteral("manual"));
    mode->addItem(tr("ROS Agent 自动校正"), QStringLiteral("remote_clock"));
    const auto selected = mode->findData(
        definition.value(QStringLiteral("mode"), QStringLiteral("source")));
    mode->setCurrentIndex(selected < 0 ? 0 : selected);
    rules_->setCellWidget(row, 1, mode);

    auto* offset = new QDoubleSpinBox(rules_);
    offset->setRange(-604'800'000.0, 604'800'000.0);
    offset->setDecimals(6);
    offset->setSuffix(QStringLiteral(" ms"));
    offset->setValue(
        definition.value(QStringLiteral("offset_ns")).toLongLong() / 1'000'000.0);
    offset->setEnabled(mode->currentData().toString() == QStringLiteral("manual"));
    connect(mode, &QComboBox::currentIndexChanged, offset, [mode, offset] {
        offset->setEnabled(
            mode->currentData().toString() == QStringLiteral("manual"));
    });
    rules_->setCellWidget(row, 2, offset);
}

}  // namespace lab::ui

#include "ui/alerts_widget.hpp"

#include <QAbstractItemView>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QVariantMap>

#include <set>

namespace lab::ui {

AlertsWidget::AlertsWidget(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("alertsWidget"));

    markerText_ = new QLineEdit(this);
    markerText_->setObjectName(QStringLiteral("markerText"));
    markerText_->setPlaceholderText(tr("例如：开始旋转测试"));
    markerText_->setMaxLength(1024);
    auto *markerButton = new QPushButton(tr("添加 Marker"), this);
    markerButton->setObjectName(QStringLiteral("addMarkerButton"));
    auto *markerRow = new QHBoxLayout;
    markerRow->addWidget(new QLabel(tr("当前时刻说明"), this));
    markerRow->addWidget(markerText_, 1);
    markerRow->addWidget(markerButton);

    rules_ = new QTableWidget(0, 6, this);
    rules_->setObjectName(QStringLiteral("alertRulesTable"));
    rules_->setHorizontalHeaderLabels(
        {tr("规则名"), tr("字段"), tr("条件"), tr("阈值"), tr("回差"), tr("说明")});
    rules_->setSelectionBehavior(QAbstractItemView::SelectRows);
    rules_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    rules_->verticalHeader()->hide();
    rules_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    rules_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    rules_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);

    auto *addRuleButton = new QPushButton(tr("添加规则"), this);
    addRuleButton->setObjectName(QStringLiteral("addAlertRuleButton"));
    removeButton_ = new QPushButton(tr("删除选中"), this);
    removeButton_->setObjectName(QStringLiteral("removeAlertRuleButton"));
    auto *applyButton = new QPushButton(tr("应用告警规则"), this);
    applyButton->setObjectName(QStringLiteral("applyAlertRulesButton"));
    auto *ruleButtons = new QHBoxLayout;
    ruleButtons->addWidget(addRuleButton);
    ruleButtons->addWidget(removeButton_);
    ruleButtons->addStretch(1);
    ruleButtons->addWidget(applyButton);

    status_ = new QLabel(tr("尚未配置阈值告警"), this);
    status_->setObjectName(QStringLiteral("alertRulesStatus"));
    status_->setWordWrap(true);

    auto *ruleBox = new QGroupBox(tr("阈值告警（越界时只触发一次，回到回差范围后复位）"), this);
    auto *ruleLayout = new QVBoxLayout(ruleBox);
    ruleLayout->addWidget(rules_);
    ruleLayout->addLayout(ruleButtons);
    ruleLayout->addWidget(status_);

    events_ = new QTableWidget(0, 4, this);
    events_->setObjectName(QStringLiteral("timelineEventsTable"));
    events_->setHorizontalHeaderLabels({tr("时间"), tr("类型"), tr("来源"), tr("说明")});
    events_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    events_->setSelectionBehavior(QAbstractItemView::SelectRows);
    events_->verticalHeader()->hide();
    events_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    events_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    events_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    events_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    auto *eventBox = new QGroupBox(tr("Session 时间线事件"), this);
    auto *eventLayout = new QVBoxLayout(eventBox);
    eventLayout->addWidget(events_);

    auto *splitter = new QSplitter(Qt::Vertical, this);
    splitter->addWidget(ruleBox);
    splitter->addWidget(eventBox);
    splitter->setStretchFactor(1, 1);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(markerRow);
    layout->addWidget(splitter, 1);

    const auto addMarker = [this] {
        const auto text = markerText_->text().trimmed();
        if (text.isEmpty()) return;
        emit addMarkerRequested(text);
        markerText_->clear();
    };
    connect(markerButton, &QPushButton::clicked, this, addMarker);
    connect(markerText_, &QLineEdit::returnPressed, this, addMarker);
    connect(addRuleButton, &QPushButton::clicked, this, [this] { appendRule(); });
    connect(removeButton_, &QPushButton::clicked, this, &AlertsWidget::removeSelectedRules);
    connect(applyButton, &QPushButton::clicked, this,
            [this] { emit applyRequested(definitions()); });
    connect(rules_, &QTableWidget::itemSelectionChanged, this,
            [this] { removeButton_->setEnabled(!rules_->selectedItems().isEmpty()); });

    appendRule();
    removeButton_->setEnabled(false);
}

QVariantList AlertsWidget::definitions() const {
    QVariantList result;
    for (int row = 0; row < rules_->rowCount(); ++row) {
        const auto text = [this, row](int column) {
            const auto *item = rules_->item(row, column);
            return item ? item->text().trimmed() : QString{};
        };
        const auto name = text(0);
        const auto field = text(1);
        const auto message = text(5);
        const auto *comparison = qobject_cast<QComboBox *>(rules_->cellWidget(row, 2));
        const auto *threshold = qobject_cast<QDoubleSpinBox *>(rules_->cellWidget(row, 3));
        const auto *hysteresis = qobject_cast<QDoubleSpinBox *>(rules_->cellWidget(row, 4));
        if (name.isEmpty() && field.isEmpty() && message.isEmpty()) continue;
        QVariantMap definition;
        definition.insert(QStringLiteral("name"), name);
        definition.insert(QStringLiteral("field"), field);
        definition.insert(QStringLiteral("comparison"), comparison
                                                            ? comparison->currentData().toString()
                                                            : QStringLiteral("above"));
        definition.insert(QStringLiteral("threshold"), threshold ? threshold->value() : 0.0);
        definition.insert(QStringLiteral("hysteresis"), hysteresis ? hysteresis->value() : 0.0);
        definition.insert(QStringLiteral("message"), message);
        result.push_back(definition);
    }
    return result;
}

void AlertsWidget::setDefinitions(const QVariantList &definitions) {
    rules_->setRowCount(0);
    for (const auto &value : definitions)
        appendRule(value.toMap());
    if (rules_->rowCount() == 0) appendRule();
}

void AlertsWidget::setTimelineEvents(const QVariantList &events) {
    events_->setRowCount(events.size());
    for (int row = 0; row < events.size(); ++row) {
        const auto event = events[row].toMap();
        const auto timestamp = event.value(QStringLiteral("timestamp_ns")).toLongLong();
        const auto time = QDateTime::fromMSecsSinceEpoch(timestamp / 1'000'000)
                              .toString(QStringLiteral("HH:mm:ss.zzz"));
        const auto category = event.value(QStringLiteral("category")).toString();
        const QStringList values{time,
                                 category == QStringLiteral("alert") ? tr("告警") : tr("Marker"),
                                 event.value(QStringLiteral("source_id")).toString(),
                                 event.value(QStringLiteral("message")).toString()};
        for (int column = 0; column < values.size(); ++column) {
            auto *item = new QTableWidgetItem(values[column]);
            item->setToolTip(column == 0 ? tr("时间戳：%1 ns").arg(timestamp) : values[column]);
            if (category == QStringLiteral("alert")) {
                item->setForeground(QColor(QStringLiteral("#d99145")));
            }
            events_->setItem(row, column, item);
        }
    }
    if (events_->rowCount() > 0) events_->scrollToBottom();
}

void AlertsWidget::showConfigurationResult(bool success, const QStringList &messages) {
    status_->setText(messages.isEmpty() ? (success ? tr("告警规则已应用") : tr("告警规则无效"))
                                        : messages.join(QLatin1Char('\n')));
    status_->setStyleSheet(success ? QStringLiteral("color: #74c991;")
                                   : QStringLiteral("color: #ff8d8d;"));
}

void AlertsWidget::appendRule(const QVariantMap &definition) {
    const auto row = rules_->rowCount();
    rules_->insertRow(row);
    rules_->setItem(row, 0,
                    new QTableWidgetItem(definition.value(QStringLiteral("name")).toString()));
    rules_->setItem(row, 1,
                    new QTableWidgetItem(definition.value(QStringLiteral("field")).toString()));
    auto *comparison = new QComboBox(rules_);
    comparison->addItem(tr("高于"), QStringLiteral("above"));
    comparison->addItem(tr("低于"), QStringLiteral("below"));
    const auto comparisonIndex = comparison->findData(
        definition.value(QStringLiteral("comparison"), QStringLiteral("above")));
    comparison->setCurrentIndex(comparisonIndex < 0 ? 0 : comparisonIndex);
    rules_->setCellWidget(row, 2, comparison);
    auto *threshold = new QDoubleSpinBox(rules_);
    threshold->setRange(-1e15, 1e15);
    threshold->setDecimals(8);
    threshold->setValue(definition.value(QStringLiteral("threshold")).toDouble());
    rules_->setCellWidget(row, 3, threshold);
    auto *hysteresis = new QDoubleSpinBox(rules_);
    hysteresis->setRange(0.0, 1e15);
    hysteresis->setDecimals(8);
    hysteresis->setValue(definition.value(QStringLiteral("hysteresis")).toDouble());
    rules_->setCellWidget(row, 4, hysteresis);
    rules_->setItem(row, 5,
                    new QTableWidgetItem(definition.value(QStringLiteral("message")).toString()));
    rules_->setCurrentCell(row, 0);
}

void AlertsWidget::removeSelectedRules() {
    std::set<int, std::greater<>> rows;
    for (const auto *item : rules_->selectedItems())
        rows.insert(item->row());
    for (const auto row : rows)
        rules_->removeRow(row);
    if (rules_->rowCount() == 0) appendRule();
}

} // namespace lab::ui

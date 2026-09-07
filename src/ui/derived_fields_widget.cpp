#include "ui/derived_fields_widget.hpp"

#include <QAbstractItemView>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVariantMap>
#include <QVBoxLayout>

#include <functional>
#include <set>

namespace lab::ui {

DerivedFieldsWidget::DerivedFieldsWidget(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("derivedFieldsWidget"));

    auto* explanation = new QLabel(
        tr("用已有数值字段生成新曲线。支持 +、-、*、/、括号和 "
           "abs / sqrt / min / max / pow / clamp。复杂字段名请放在反引号中，"
           "例如 `serial:COM5.voltage`。"),
        this);
    explanation->setObjectName(QStringLiteral("secondaryText"));
    explanation->setWordWrap(true);

    table_ = new QTableWidget(0, 3, this);
    table_->setObjectName(QStringLiteral("derivedFieldsTable"));
    table_->setHorizontalHeaderLabels({tr("派生字段名"), tr("表达式"), tr("单位")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);

    auto* addButton = new QPushButton(tr("添加一行"), this);
    addButton->setObjectName(QStringLiteral("addDerivedFieldButton"));
    removeButton_ = new QPushButton(tr("删除选中"), this);
    removeButton_->setObjectName(QStringLiteral("removeDerivedFieldButton"));
    auto* applyButton = new QPushButton(tr("应用派生变量"), this);
    applyButton->setObjectName(QStringLiteral("applyDerivedFieldsButton"));

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(addButton);
    buttons->addWidget(removeButton_);
    buttons->addStretch(1);
    buttons->addWidget(applyButton);

    status_ = new QLabel(tr("尚未配置派生变量"), this);
    status_->setObjectName(QStringLiteral("derivedFieldsStatus"));
    status_->setWordWrap(true);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(explanation);
    layout->addWidget(table_, 1);
    layout->addLayout(buttons);
    layout->addWidget(status_);

    connect(addButton, &QPushButton::clicked, this, [this] { appendEmptyRow(); });
    connect(removeButton_, &QPushButton::clicked,
            this, &DerivedFieldsWidget::removeSelectedRows);
    connect(applyButton, &QPushButton::clicked, this, [this] {
        emit applyRequested(definitions());
    });
    connect(table_, &QTableWidget::itemSelectionChanged, this, [this] {
        removeButton_->setEnabled(!table_->selectedItems().isEmpty());
    });

    appendEmptyRow();
    removeButton_->setEnabled(false);
}

QVariantList DerivedFieldsWidget::definitions() const {
    QVariantList result;
    for (int row = 0; row < table_->rowCount(); ++row) {
        const auto value = [this, row](int column) {
            const auto* item = table_->item(row, column);
            return item ? item->text().trimmed() : QString{};
        };
        const auto name = value(0);
        const auto expression = value(1);
        const auto unit = value(2);
        if (name.isEmpty() && expression.isEmpty() && unit.isEmpty()) continue;
        QVariantMap definition;
        definition.insert(QStringLiteral("name"), name);
        definition.insert(QStringLiteral("expression"), expression);
        definition.insert(QStringLiteral("unit"), unit);
        result.push_back(definition);
    }
    return result;
}

void DerivedFieldsWidget::setDefinitions(const QVariantList& definitions) {
    table_->setRowCount(0);
    for (const auto& value : definitions) {
        const auto definition = value.toMap();
        const auto row = table_->rowCount();
        table_->insertRow(row);
        table_->setItem(
            row, 0, new QTableWidgetItem(definition.value(QStringLiteral("name")).toString()));
        table_->setItem(
            row,
            1,
            new QTableWidgetItem(definition.value(QStringLiteral("expression")).toString()));
        table_->setItem(
            row, 2, new QTableWidgetItem(definition.value(QStringLiteral("unit")).toString()));
    }
    if (table_->rowCount() == 0) appendEmptyRow();
}

void DerivedFieldsWidget::showConfigurationResult(
    bool success,
    const QStringList& messages) {
    status_->setText(messages.isEmpty()
                         ? (success ? tr("派生变量已应用") : tr("派生变量配置无效"))
                         : messages.join(QLatin1Char('\n')));
    status_->setStyleSheet(success ? QStringLiteral("color: #74c991;")
                                   : QStringLiteral("color: #ff8d8d;"));
}

void DerivedFieldsWidget::appendEmptyRow() {
    const auto row = table_->rowCount();
    table_->insertRow(row);
    table_->setItem(row, 0, new QTableWidgetItem);
    table_->setItem(row, 1, new QTableWidgetItem);
    table_->setItem(row, 2, new QTableWidgetItem);
    table_->setCurrentCell(row, 0);
}

void DerivedFieldsWidget::removeSelectedRows() {
    std::set<int, std::greater<>> rows;
    for (const auto* item : table_->selectedItems()) rows.insert(item->row());
    for (const auto row : rows) table_->removeRow(row);
    if (table_->rowCount() == 0) appendEmptyRow();
}

}  // namespace lab::ui

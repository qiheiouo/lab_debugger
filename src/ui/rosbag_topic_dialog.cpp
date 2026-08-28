#include "ui/rosbag_topic_dialog.hpp"

#include <QAbstractItemView>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QVariantMap>

namespace lab::ui {
namespace {

constexpr int nameRole = Qt::UserRole;
constexpr int typeRole = Qt::UserRole + 1;
constexpr int supportedRole = Qt::UserRole + 2;
constexpr int countRole = Qt::UserRole + 3;
constexpr int structuredRole = Qt::UserRole + 4;

}  // namespace

RosbagTopicDialog::RosbagTopicDialog(QString source,
                                     QString destination,
                                     QVariantList topics,
                                     QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("选择要导入的 ROS Topic"));
    resize(860, 560);
    setMinimumSize(680, 420);

    auto* root = new QVBoxLayout(this);
    auto* introduction = new QLabel(
        tr("已读取 rosbag2 目录。请选择需要回放的 Topic；常见消息可直接生成曲线，其他消息仍安全保留原始 CDR。"),
        this);
    introduction->setWordWrap(true);
    root->addWidget(introduction);

    auto* paths = new QLabel(tr("来源：%1\n目标：%2").arg(source, destination), this);
    paths->setTextInteractionFlags(Qt::TextSelectableByMouse);
    paths->setWordWrap(true);
    paths->setStyleSheet(QStringLiteral("color: #8794a6;"));
    root->addWidget(paths);

    search_ = new QLineEdit(this);
    search_->setPlaceholderText(tr("搜索 Topic 名或消息类型"));
    search_->setClearButtonEnabled(true);
    root->addWidget(search_);

    topics_ = new QTableWidget(static_cast<int>(topics.size()), 5, this);
    topics_->setHorizontalHeaderLabels(
        {tr("Topic"), tr("消息类型"), tr("消息数"), tr("序列化"), tr("回放能力")});
    topics_->setSelectionBehavior(QAbstractItemView::SelectRows);
    topics_->setSelectionMode(QAbstractItemView::SingleSelection);
    topics_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    topics_->setAlternatingRowColors(true);
    topics_->verticalHeader()->setVisible(false);
    topics_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    topics_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    topics_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    topics_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    topics_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);

    topics_->blockSignals(true);
    for (int row = 0; row < topics.size(); ++row) {
        const auto topic = topics.at(row).toMap();
        const auto name = topic.value(QStringLiteral("name")).toString();
        const auto type = topic.value(QStringLiteral("type")).toString();
        const auto format = topic.value(QStringLiteral("serialization_format")).toString();
        const auto count = topic.value(QStringLiteral("message_count")).toULongLong();
        const auto supported = format == QStringLiteral("cdr");
        const auto structured =
            topic.value(QStringLiteral("structured_fields")).toBool();

        auto* nameItem = new QTableWidgetItem(name);
        nameItem->setData(nameRole, name);
        nameItem->setData(typeRole, type);
        nameItem->setData(supportedRole, supported);
        nameItem->setData(countRole, count);
        nameItem->setData(structuredRole, structured);
        if (supported) {
            nameItem->setFlags(nameItem->flags() | Qt::ItemIsUserCheckable);
            nameItem->setCheckState(Qt::Checked);
        } else {
            nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEnabled);
            nameItem->setToolTip(tr("当前版本只支持 CDR 序列化"));
        }
        topics_->setItem(row, 0, nameItem);
        topics_->setItem(row, 1, new QTableWidgetItem(type));
        auto* countItem = new QTableWidgetItem;
        countItem->setData(Qt::DisplayRole, count);
        countItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        topics_->setItem(row, 2, countItem);
        topics_->setItem(
            row,
            3,
            new QTableWidgetItem(supported ? tr("CDR（支持）")
                                           : tr("%1（不支持）").arg(format)));
        topics_->setItem(
            row,
            4,
            new QTableWidgetItem(
                !supported ? tr("不可导入")
                           : structured ? tr("原始 CDR + 曲线")
                                        : tr("仅原始 CDR")));
    }
    topics_->blockSignals(false);
    root->addWidget(topics_, 1);

    auto* actions = new QHBoxLayout;
    auto* selectVisible = new QPushButton(tr("全选当前结果"), this);
    auto* clear = new QPushButton(tr("清空选择"), this);
    summary_ = new QLabel(this);
    auto* cancel = new QPushButton(tr("取消"), this);
    importButton_ = new QPushButton(tr("导入选中 Topic"), this);
    importButton_->setDefault(true);
    actions->addWidget(selectVisible);
    actions->addWidget(clear);
    actions->addSpacing(12);
    actions->addWidget(summary_, 1);
    actions->addWidget(cancel);
    actions->addWidget(importButton_);
    root->addLayout(actions);

    connect(search_, &QLineEdit::textChanged, this, &RosbagTopicDialog::filterRows);
    connect(topics_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem*) {
        updateSelectionSummary();
    });
    connect(selectVisible, &QPushButton::clicked, this, [this] {
        setVisibleSelection(Qt::Checked);
    });
    connect(clear, &QPushButton::clicked, this, [this] {
        search_->clear();
        setVisibleSelection(Qt::Unchecked);
    });
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(importButton_, &QPushButton::clicked, this, &QDialog::accept);
    updateSelectionSummary();
}

QVariantList RosbagTopicDialog::selectedTopics() const {
    QVariantList result;
    for (int row = 0; row < topics_->rowCount(); ++row) {
        const auto* item = topics_->item(row, 0);
        if (item == nullptr || item->checkState() != Qt::Checked ||
            !item->data(supportedRole).toBool()) {
            continue;
        }
        QVariantMap selected;
        selected.insert(QStringLiteral("name"), item->data(nameRole).toString());
        selected.insert(QStringLiteral("type"), item->data(typeRole).toString());
        result.push_back(selected);
    }
    return result;
}

void RosbagTopicDialog::filterRows(const QString& text) {
    const auto query = text.trimmed();
    for (int row = 0; row < topics_->rowCount(); ++row) {
        const auto* item = topics_->item(row, 0);
        const auto matches = query.isEmpty() ||
                             item->data(nameRole).toString().contains(
                                 query, Qt::CaseInsensitive) ||
                             item->data(typeRole).toString().contains(
                                 query, Qt::CaseInsensitive);
        topics_->setRowHidden(row, !matches);
    }
}

void RosbagTopicDialog::setVisibleSelection(Qt::CheckState state) {
    topics_->blockSignals(true);
    for (int row = 0; row < topics_->rowCount(); ++row) {
        auto* item = topics_->item(row, 0);
        if (!topics_->isRowHidden(row) && item->data(supportedRole).toBool()) {
            item->setCheckState(state);
        }
    }
    topics_->blockSignals(false);
    updateSelectionSummary();
}

void RosbagTopicDialog::updateSelectionSummary() {
    quint64 selectedMessages{};
    int selectedCount{};
    int structuredCount{};
    for (int row = 0; row < topics_->rowCount(); ++row) {
        const auto* item = topics_->item(row, 0);
        if (item != nullptr && item->checkState() == Qt::Checked &&
            item->data(supportedRole).toBool()) {
            ++selectedCount;
            selectedMessages += item->data(countRole).toULongLong();
            if (item->data(structuredRole).toBool()) ++structuredCount;
        }
    }
    summary_->setText(tr("已选 %1 个 Topic，%2 条消息，%3 个可生成曲线")
                          .arg(selectedCount)
                          .arg(selectedMessages)
                          .arg(structuredCount));
    importButton_->setEnabled(selectedCount > 0);
}

}  // namespace lab::ui

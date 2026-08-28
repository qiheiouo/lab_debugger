#include "ui/rosbag_topic_dialog.hpp"

#include <QApplication>
#include <QPushButton>
#include <QTableWidget>
#include <QVariantMap>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

QVariantMap topic(QString name, QString type, QString format, quint64 count) {
    QVariantMap result;
    result.insert(QStringLiteral("name"), std::move(name));
    result.insert(QStringLiteral("type"), std::move(type));
    result.insert(QStringLiteral("serialization_format"), std::move(format));
    result.insert(QStringLiteral("message_count"), count);
    return result;
}

}  // namespace

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    try {
        const QVariantList topics{
            topic(QStringLiteral("/temperature"),
                  QStringLiteral("std_msgs/msg/Float64"),
                  QStringLiteral("cdr"),
                  20),
            topic(QStringLiteral("/status"),
                  QStringLiteral("std_msgs/msg/String"),
                  QStringLiteral("cdr"),
                  3),
            topic(QStringLiteral("/unsupported"),
                  QStringLiteral("custom/msg/Json"),
                  QStringLiteral("json"),
                  7)};
        lab::ui::RosbagTopicDialog dialog(
            QStringLiteral("bag"), QStringLiteral("session"), topics);

        const auto* table = dialog.findChild<QTableWidget*>();
        require(table != nullptr && table->rowCount() == 3,
                "Topic selection table contains every inspected Topic");
        require(dialog.selectedTopics().size() == 2,
                "all supported CDR Topics are selected by default");
        require(!(table->item(2, 0)->flags() & Qt::ItemIsEnabled) &&
                    table->item(2, 0)->checkState() != Qt::Checked,
                "unsupported serialization is visible but cannot be selected");

        table->item(0, 0)->setCheckState(Qt::Unchecked);
        const auto selected = dialog.selectedTopics();
        require(selected.size() == 1 &&
                    selected.front().toMap().value(QStringLiteral("name")).toString() ==
                        QStringLiteral("/status"),
                "checkbox state produces an exact name/type selection");
        table->item(1, 0)->setCheckState(Qt::Unchecked);

        const auto buttons = dialog.findChildren<QPushButton*>();
        const auto importButton = std::find_if(
            buttons.begin(), buttons.end(), [](const auto* button) {
                return button->text().contains(QStringLiteral("导入选中"));
            });
        require(importButton != buttons.end() && !(*importButton)->isEnabled(),
                "dialog prevents an empty Topic import");
        std::cout << "Lab Debugger rosbag2 Topic dialog tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Lab Debugger rosbag2 Topic dialog tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}

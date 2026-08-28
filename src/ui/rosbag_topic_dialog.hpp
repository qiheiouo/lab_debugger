#pragma once

#include <QDialog>
#include <QString>
#include <QVariantList>

class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace lab::ui {

class RosbagTopicDialog final : public QDialog {
    Q_OBJECT

public:
    RosbagTopicDialog(QString source,
                      QString destination,
                      QVariantList topics,
                      QWidget* parent = nullptr);

    [[nodiscard]] QVariantList selectedTopics() const;

private:
    void filterRows(const QString& text);
    void setVisibleSelection(Qt::CheckState state);
    void updateSelectionSummary();

    QLineEdit* search_{};
    QTableWidget* topics_{};
    QLabel* summary_{};
    QPushButton* importButton_{};
};

}  // namespace lab::ui

#pragma once

#include <QStringList>
#include <QVariantList>
#include <QWidget>

class QLabel;
class QPushButton;
class QTableWidget;

namespace lab::ui {

class DerivedFieldsWidget final : public QWidget {
    Q_OBJECT

public:
    explicit DerivedFieldsWidget(QWidget* parent = nullptr);

    [[nodiscard]] QVariantList definitions() const;

public slots:
    void setDefinitions(const QVariantList& definitions);
    void showConfigurationResult(bool success, const QStringList& messages);

signals:
    void applyRequested(QVariantList definitions);

private:
    void appendEmptyRow();
    void removeSelectedRows();

    QTableWidget* table_{};
    QLabel* status_{};
    QPushButton* removeButton_{};
};

}  // namespace lab::ui

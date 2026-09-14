#pragma once

#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QWidget>

class QLabel;
class QPushButton;
class QTableWidget;

namespace lab::ui {

class TimeAlignmentWidget final : public QWidget {
    Q_OBJECT

public:
    explicit TimeAlignmentWidget(QWidget* parent = nullptr);

    [[nodiscard]] QVariantList definitions() const;

public slots:
    void setDefinitions(const QVariantList& definitions);
    void setStatuses(const QVariantList& statuses);
    void showConfigurationResult(bool success, const QStringList& messages);

signals:
    void applyRequested(QVariantList definitions);

private slots:
    void addRule();
    void removeSelectedRules();

private:
    void appendRule(const QVariantMap& definition = {});

    QTableWidget* rules_{};
    QTableWidget* statuses_{};
    QPushButton* removeButton_{};
    QLabel* status_{};
};

}  // namespace lab::ui

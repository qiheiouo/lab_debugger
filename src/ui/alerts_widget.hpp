#pragma once

#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace lab::ui {

class AlertsWidget final : public QWidget {
    Q_OBJECT

  public:
    explicit AlertsWidget(QWidget *parent = nullptr);

    [[nodiscard]] QVariantList definitions() const;

  public slots:
    void setDefinitions(const QVariantList &definitions);
    void setTimelineEvents(const QVariantList &events);
    void showConfigurationResult(bool success, const QStringList &messages);

  signals:
    void addMarkerRequested(QString message);
    void applyRequested(QVariantList definitions);

  private:
    void appendRule(const QVariantMap &definition = {});
    void removeSelectedRules();

    QLineEdit *markerText_{};
    QTableWidget *rules_{};
    QTableWidget *events_{};
    QLabel *status_{};
    QPushButton *removeButton_{};
};

} // namespace lab::ui

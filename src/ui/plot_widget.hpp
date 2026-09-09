#pragma once

#include "lab/core/time_series_store.hpp"

#include <QStringList>
#include <QVariantList>
#include <QWidget>

class QCheckBox;
class QDoubleSpinBox;
class QLineEdit;
class QListWidget;
class QSpinBox;
class QTableWidget;
class QTimer;

namespace lab::ui {

class PlotCanvas;

class PlotWidget final : public QWidget {
    Q_OBJECT

public:
    explicit PlotWidget(
        const lab::core::TimeSeriesStore* store,
        QWidget* parent = nullptr);
    [[nodiscard]] QStringList fieldNames() const;

public slots:
    void useProtocolFields(const QStringList& fields);
    void useExternalFields(const QStringList& fields);
    void setTimelineEvents(const QVariantList& events);

signals:
    void fieldsChanged(QStringList fields);

private slots:
    void applyFields();
    void updatePlotAndStatistics();
    void updateSelectedFields();
    void updateRanges();

private:
    [[nodiscard]] QStringList configuredFields() const;
    [[nodiscard]] QStringList visibleFieldNames() const;
    void rebuildFieldControls(bool notifyParser);
    void rebuildVisibleFields(const QStringList& fields, bool preserveChecks);

    const lab::core::TimeSeriesStore* store_;
    PlotCanvas* canvas_{};
    QLineEdit* fields_{};
    QListWidget* visibleFields_{};
    QSpinBox* timeWindow_{};
    QCheckBox* paused_{};
    QCheckBox* autoY_{};
    QDoubleSpinBox* yMinimum_{};
    QDoubleSpinBox* yMaximum_{};
    QTableWidget* statistics_{};
    QTimer* refreshTimer_{};
};

}  // namespace lab::ui

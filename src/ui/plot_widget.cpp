#include "ui/plot_widget.hpp"

#include "lab/core/timestamp.hpp"

#include <QCheckBox>
#include <QColor>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace lab::ui {
namespace {

const std::vector<QColor> kColors{
    QColor("#58a6ff"), QColor("#5ad49b"), QColor("#f2cc60"),
    QColor("#ff7b72"), QColor("#bc8cff"), QColor("#39c5cf"),
    QColor("#ffa657"), QColor("#d2a8ff"), QColor("#7ee787"),
    QColor("#ff9bce")};

}  // namespace

class PlotCanvas final : public QWidget {
public:
    explicit PlotCanvas(const lab::core::TimeSeriesStore* store, QWidget* parent = nullptr)
        : QWidget(parent), store_(store) {
        setMinimumSize(500, 280);
        setMouseTracking(true);
        setAutoFillBackground(true);
    }

    void setFields(QStringList fields) {
        fields_ = std::move(fields);
        update();
    }

    void setTimeWindow(double seconds) {
        timeWindowSeconds_ = std::clamp(seconds, 0.5, 600.0);
        update();
    }

    void setPaused(bool paused) {
        if (paused && !paused_) {
            frozenEnd_ = newestTimestamp();
        }
        paused_ = paused;
        update();
    }

    void setYRange(bool automatic, double minimum, double maximum) {
        automaticY_ = automatic;
        manualMinimum_ = minimum;
        manualMaximum_ = maximum;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), QColor("#111722"));

        const QRectF plotRect(64, 20, width() - 86, height() - 62);
        if (plotRect.width() <= 1 || plotRect.height() <= 1) {
            return;
        }

        auto end = paused_ ? frozenEnd_ : newestTimestamp();
        if (end == 0) {
            painter.setPen(QColor("#9aa4b2"));
            painter.drawText(rect(), Qt::AlignCenter, tr("等待换行分隔的数值数据…"));
            return;
        }
        const auto windowNs = static_cast<lab::core::Timestamp>(timeWindowSeconds_ * 1e9);
        const auto start = end - windowNs;

        std::vector<std::vector<lab::core::DataPoint>> series;
        series.reserve(fields_.size());
        double minimum = std::numeric_limits<double>::max();
        double maximum = std::numeric_limits<double>::lowest();
        for (const auto& field : fields_) {
            auto points = store_->downsampleMinMax(
                field.toStdString(),
                start,
                static_cast<std::size_t>(std::max(100.0, plotRect.width() * 2.0)));
            for (const auto& point : points) {
                minimum = std::min(minimum, point.value);
                maximum = std::max(maximum, point.value);
            }
            series.push_back(std::move(points));
        }

        if (!automaticY_) {
            minimum = manualMinimum_;
            maximum = manualMaximum_;
        }
        if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum > maximum) {
            minimum = -1.0;
            maximum = 1.0;
        }
        if (std::abs(maximum - minimum) < 1e-12) {
            const auto padding = std::max(1.0, std::abs(maximum) * 0.1);
            minimum -= padding;
            maximum += padding;
        } else if (automaticY_) {
            const auto padding = (maximum - minimum) * 0.08;
            minimum -= padding;
            maximum += padding;
        }

        painter.setPen(QPen(QColor("#263244"), 1));
        for (int line = 0; line <= 5; ++line) {
            const auto y = plotRect.top() + plotRect.height() * line / 5.0;
            painter.drawLine(QPointF(plotRect.left(), y), QPointF(plotRect.right(), y));
            const auto value = maximum - (maximum - minimum) * line / 5.0;
            painter.setPen(QColor("#8090a4"));
            painter.drawText(QRectF(2, y - 10, 56, 20), Qt::AlignRight | Qt::AlignVCenter,
                             QString::number(value, 'g', 4));
            painter.setPen(QPen(QColor("#263244"), 1));
        }
        for (int line = 0; line <= 5; ++line) {
            const auto x = plotRect.left() + plotRect.width() * line / 5.0;
            painter.drawLine(QPointF(x, plotRect.top()), QPointF(x, plotRect.bottom()));
            const auto seconds = -timeWindowSeconds_ + timeWindowSeconds_ * line / 5.0;
            painter.setPen(QColor("#8090a4"));
            painter.drawText(QRectF(x - 35, plotRect.bottom() + 5, 70, 20), Qt::AlignCenter,
                             QString::number(seconds, 'g', 3) + QStringLiteral(" s"));
            painter.setPen(QPen(QColor("#263244"), 1));
        }

        painter.save();
        painter.setClipRect(plotRect);
        for (std::size_t fieldIndex = 0; fieldIndex < series.size(); ++fieldIndex) {
            const auto& points = series[fieldIndex];
            if (points.empty()) {
                continue;
            }
            QPainterPath path;
            bool started = false;
            for (const auto& point : points) {
                const auto xRatio = static_cast<double>(point.timestamp - start) /
                                    static_cast<double>(windowNs);
                const auto yRatio = (point.value - minimum) / (maximum - minimum);
                const QPointF position(
                    plotRect.left() + xRatio * plotRect.width(),
                    plotRect.bottom() - yRatio * plotRect.height());
                if (!started) {
                    path.moveTo(position);
                    started = true;
                } else {
                    path.lineTo(position);
                }
            }
            painter.setPen(QPen(kColors[fieldIndex % kColors.size()], 1.6));
            painter.drawPath(path);
        }
        painter.restore();

        qreal legendX = plotRect.left();
        for (int index = 0; index < fields_.size(); ++index) {
            painter.setPen(kColors[static_cast<std::size_t>(index) % kColors.size()]);
            const auto label = fields_[index];
            painter.drawText(QPointF(legendX, 14), label);
            legendX += painter.fontMetrics().horizontalAdvance(label) + 20;
        }
    }

    void wheelEvent(QWheelEvent* event) override {
        const auto factor = event->angleDelta().y() > 0 ? 0.8 : 1.25;
        setTimeWindow(timeWindowSeconds_ * factor);
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        const QRectF plotRect(64, 20, width() - 86, height() - 62);
        if (!plotRect.contains(event->position()) || fields_.isEmpty()) {
            return;
        }
        const auto end = paused_ ? frozenEnd_ : newestTimestamp();
        const auto windowNs = static_cast<lab::core::Timestamp>(timeWindowSeconds_ * 1e9);
        const auto start = end - windowNs;
        const auto cursorTime = start + static_cast<lab::core::Timestamp>(
            (event->position().x() - plotRect.left()) / plotRect.width() * windowNs);
        QStringList lines;
        lines << tr("相对时间 %1 s").arg(lab::core::secondsBetween(end, cursorTime), 0, 'f', 3);
        for (const auto& field : fields_) {
            const auto points = store_->snapshot(field.toStdString(), start);
            if (points.empty()) {
                continue;
            }
            const auto iterator = std::lower_bound(
                points.begin(), points.end(), cursorTime,
                [](const auto& point, auto timestamp) { return point.timestamp < timestamp; });
            const auto& point = iterator == points.end() ? points.back() : *iterator;
            lines << QStringLiteral("%1 = %2").arg(field).arg(point.value, 0, 'g', 8);
        }
        QToolTip::showText(event->globalPosition().toPoint(), lines.join('\n'), this);
    }

private:
    [[nodiscard]] lab::core::Timestamp newestTimestamp() const {
        lab::core::Timestamp newest = 0;
        for (const auto& field : fields_) {
            const auto points = store_->snapshot(field.toStdString());
            if (!points.empty()) {
                newest = std::max(newest, points.back().timestamp);
            }
        }
        return newest;
    }

    const lab::core::TimeSeriesStore* store_;
    QStringList fields_;
    double timeWindowSeconds_{10.0};
    bool paused_{};
    bool automaticY_{true};
    double manualMinimum_{-1.0};
    double manualMaximum_{1.0};
    lab::core::Timestamp frozenEnd_{};
};

PlotWidget::PlotWidget(
    const lab::core::TimeSeriesStore* store,
    QWidget* parent)
    : QWidget(parent), store_(store) {
    fields_ = new QLineEdit(QStringLiteral("speed,current,voltage"), this);
    auto* applyButton = new QPushButton(tr("应用字段"), this);
    visibleFields_ = new QListWidget(this);
    visibleFields_->setMaximumHeight(110);

    timeWindow_ = new QSpinBox(this);
    timeWindow_->setRange(1, 600);
    timeWindow_->setValue(10);
    timeWindow_->setSuffix(QStringLiteral(" s"));
    paused_ = new QCheckBox(tr("暂停曲线"), this);
    autoY_ = new QCheckBox(tr("自动 Y 范围"), this);
    autoY_->setChecked(true);
    yMinimum_ = new QDoubleSpinBox(this);
    yMaximum_ = new QDoubleSpinBox(this);
    for (auto* spin : {yMinimum_, yMaximum_}) {
        spin->setRange(-1e12, 1e12);
        spin->setDecimals(4);
        spin->setEnabled(false);
    }
    yMinimum_->setValue(-1.0);
    yMaximum_->setValue(1.0);

    auto* fieldRow = new QHBoxLayout;
    fieldRow->addWidget(new QLabel(tr("CSV 字段"), this));
    fieldRow->addWidget(fields_, 1);
    fieldRow->addWidget(applyButton);

    auto* controls = new QFormLayout;
    controls->addRow(tr("时间窗口"), timeWindow_);
    controls->addRow(QString(), paused_);
    controls->addRow(QString(), autoY_);
    controls->addRow(tr("Y 最小值"), yMinimum_);
    controls->addRow(tr("Y 最大值"), yMaximum_);

    auto* side = new QWidget(this);
    auto* sideLayout = new QVBoxLayout(side);
    sideLayout->addLayout(controls);
    sideLayout->addWidget(new QLabel(tr("显示字段"), side));
    sideLayout->addWidget(visibleFields_);
    sideLayout->addStretch(1);

    canvas_ = new PlotCanvas(store_, this);
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(side);
    splitter->addWidget(canvas_);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({190, 800});

    statistics_ = new QTableWidget(0, 5, this);
    statistics_->setHorizontalHeaderLabels(
        {tr("字段"), tr("Current"), tr("Min"), tr("Max"), tr("Average")});
    statistics_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    statistics_->verticalHeader()->hide();
    statistics_->setMaximumHeight(150);
    statistics_->setEditTriggers(QAbstractItemView::NoEditTriggers);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->addLayout(fieldRow);
    layout->addWidget(splitter, 1);
    layout->addWidget(statistics_);

    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(33);
    refreshTimer_->setTimerType(Qt::PreciseTimer);

    connect(applyButton, &QPushButton::clicked, this, &PlotWidget::applyFields);
    connect(fields_, &QLineEdit::returnPressed, this, &PlotWidget::applyFields);
    connect(visibleFields_, &QListWidget::itemChanged, this, &PlotWidget::updateSelectedFields);
    connect(timeWindow_, &QSpinBox::valueChanged, this, &PlotWidget::updateRanges);
    connect(paused_, &QCheckBox::toggled, this, &PlotWidget::updateRanges);
    connect(autoY_, &QCheckBox::toggled, this, &PlotWidget::updateRanges);
    connect(yMinimum_, &QDoubleSpinBox::valueChanged, this, &PlotWidget::updateRanges);
    connect(yMaximum_, &QDoubleSpinBox::valueChanged, this, &PlotWidget::updateRanges);
    connect(refreshTimer_, &QTimer::timeout, this, &PlotWidget::updatePlotAndStatistics);
    refreshTimer_->start();

    applyFields();
}

void PlotWidget::useProtocolFields(const QStringList& fields) {
    if (fields.isEmpty()) {
        return;
    }
    fields_->setText(fields.join(QStringLiteral(",")));
    applyFields();
}

void PlotWidget::applyFields() {
    const auto names = configuredFields();
    visibleFields_->blockSignals(true);
    visibleFields_->clear();
    for (const auto& name : names) {
        auto* item = new QListWidgetItem(name, visibleFields_);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
    }
    visibleFields_->blockSignals(false);
    emit fieldsChanged(names);
    updateSelectedFields();
}

void PlotWidget::updatePlotAndStatistics() {
    if (!paused_->isChecked()) {
        canvas_->update();
    }
    const auto fields = configuredFields();
    statistics_->setRowCount(fields.size());
    const auto since = lab::core::nowTimestampNs() -
                       static_cast<lab::core::Timestamp>(timeWindow_->value()) * 1'000'000'000;
    for (int row = 0; row < fields.size(); ++row) {
        statistics_->setItem(row, 0, new QTableWidgetItem(fields[row]));
        const auto stats = store_->statistics(fields[row].toStdString(), since);
        const std::array<double, 4> values = stats
            ? std::array<double, 4>{stats->current, stats->minimum, stats->maximum, stats->average}
            : std::array<double, 4>{0, 0, 0, 0};
        for (int column = 0; column < 4; ++column) {
            statistics_->setItem(
                row, column + 1,
                new QTableWidgetItem(stats ? QString::number(values[column], 'g', 8)
                                           : QStringLiteral("—")));
        }
    }
}

void PlotWidget::updateSelectedFields() {
    QStringList selected;
    for (int row = 0; row < visibleFields_->count(); ++row) {
        const auto* item = visibleFields_->item(row);
        if (item->checkState() == Qt::Checked) {
            selected.push_back(item->text());
        }
    }
    canvas_->setFields(selected);
}

void PlotWidget::updateRanges() {
    yMinimum_->setEnabled(!autoY_->isChecked());
    yMaximum_->setEnabled(!autoY_->isChecked());
    canvas_->setTimeWindow(timeWindow_->value());
    canvas_->setPaused(paused_->isChecked());
    canvas_->setYRange(autoY_->isChecked(), yMinimum_->value(), yMaximum_->value());
}

QStringList PlotWidget::configuredFields() const {
    QStringList result;
    for (const auto& part : fields_->text().split(',')) {
        const auto name = part.trimmed();
        if (!name.isEmpty() && !result.contains(name)) {
            result.push_back(name);
        }
    }
    if (result.isEmpty()) {
        result.push_back(QStringLiteral("field0"));
    }
    return result;
}

}  // namespace lab::ui

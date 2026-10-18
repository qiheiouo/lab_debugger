#include "ui/plot_widget.hpp"

#include "lab/core/time_series_csv_export.hpp"
#include "lab/core/timestamp.hpp"

#include <QCheckBox>
#include <QColor>
#include <QDoubleSpinBox>
#include <QDateTime>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSaveFile>
#include <QSet>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTimer>
#include <QToolTip>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace lab::ui {
namespace {

const std::vector<QColor> kColors{
    QColor("#58a6ff"), QColor("#5ad49b"), QColor("#f2cc60"),
    QColor("#ff7b72"), QColor("#bc8cff"), QColor("#39c5cf"),
    QColor("#ffa657"), QColor("#d2a8ff"), QColor("#7ee787"),
    QColor("#ff9bce")};

struct VisualTimelineEvent {
    lab::core::Timestamp timestamp{};
    QString category;
    QString message;
};

}  // namespace

class PlotCanvas final : public QWidget {
public:
    using Range = std::pair<lab::core::Timestamp, lab::core::Timestamp>;
    using SelectionCallback = std::function<void(std::optional<Range>)>;
    using TimeWindowCallback = std::function<void(int)>;
    using HistoryCallback = std::function<void()>;

    explicit PlotCanvas(const lab::core::TimeSeriesStore* store, QWidget* parent = nullptr)
        : QWidget(parent), store_(store) {
        setMinimumSize(500, 280);
        setMouseTracking(true);
        setAutoFillBackground(true);
        setCursor(Qt::OpenHandCursor);
    }

    void setFields(QStringList fields) {
        fields_ = std::move(fields);
        clampHistoricalEnd();
        update();
    }

    void setTimeWindow(double seconds) {
        timeWindowSeconds_ = std::clamp(seconds, 1.0, 600.0);
        clampHistoricalEnd();
        update();
    }

    void setPaused(bool paused) {
        if (paused && !paused_) {
            const auto range = visibleRange();
            frozenEnd_ = range ? range->second : newestTimestamp();
        } else if (!paused) {
            historicalEnd_.reset();
        }
        paused_ = paused;
        update();
    }

    void resetToLive() {
        historicalEnd_.reset();
        paused_ = false;
        update();
    }

    void setSelectionMode(bool enabled) {
        selectionMode_ = enabled;
        setCursor(enabled ? Qt::CrossCursor : Qt::OpenHandCursor);
    }

    void clearSelection() {
        selection_.reset();
        if (selectionChanged_) selectionChanged_(std::nullopt);
        update();
    }

    [[nodiscard]] std::optional<Range> selection() const {
        return selection_;
    }

    [[nodiscard]] std::optional<Range> visibleRange() const {
        const auto bounds = dataBounds();
        if (!bounds) return std::nullopt;
        auto end = bounds->second;
        if (historicalEnd_) {
            end = *historicalEnd_;
        } else if (paused_ && frozenEnd_ > 0) {
            end = frozenEnd_;
        }
        const auto window = windowNanoseconds();
        const auto span = bounds->second - bounds->first;
        const auto minimumEnd = span <= window ? bounds->second
                                               : bounds->first + window;
        end = std::clamp(end, minimumEnd, bounds->second);
        return Range{end - window, end};
    }

    void setSelectionCallback(SelectionCallback callback) {
        selectionChanged_ = std::move(callback);
    }

    void setTimeWindowCallback(TimeWindowCallback callback) {
        timeWindowChanged_ = std::move(callback);
    }

    void setHistoryCallback(HistoryCallback callback) {
        historyEntered_ = std::move(callback);
    }

    void setYRange(bool automatic, double minimum, double maximum) {
        automaticY_ = automatic;
        manualMinimum_ = minimum;
        manualMaximum_ = maximum;
        update();
    }

    void setTimelineEvents(const QVariantList& events) {
        events_.clear();
        events_.reserve(static_cast<std::size_t>(events.size()));
        for (const auto& value : events) {
            const auto event = value.toMap();
            events_.push_back({event.value(QStringLiteral("timestamp_ns")).toLongLong(),
                               event.value(QStringLiteral("category")).toString(),
                               event.value(QStringLiteral("message")).toString()});
        }
        clampHistoricalEnd();
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), QColor("#111722"));

        const auto area = plotRect();
        if (area.width() <= 1 || area.height() <= 1) return;
        const auto range = visibleRange();
        if (!range) {
            painter.setPen(QColor("#9aa4b2"));
            painter.drawText(rect(), Qt::AlignCenter, tr("等待数值数据…"));
            return;
        }
        const auto [start, end] = *range;
        const auto windowNs = windowNanoseconds();

        std::vector<std::vector<lab::core::DataPoint>> series;
        series.reserve(fields_.size());
        double minimum = std::numeric_limits<double>::max();
        double maximum = std::numeric_limits<double>::lowest();
        for (const auto& field : fields_) {
            auto points = store_->downsampleMinMax(
                field.toStdString(), start,
                static_cast<std::size_t>(std::max(100.0, area.width() * 2.0)), end);
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
            const auto y = area.top() + area.height() * line / 5.0;
            painter.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
            const auto value = maximum - (maximum - minimum) * line / 5.0;
            painter.setPen(QColor("#8090a4"));
            painter.drawText(QRectF(2, y - 10, 56, 20), Qt::AlignRight | Qt::AlignVCenter,
                             QString::number(value, 'g', 4));
            painter.setPen(QPen(QColor("#263244"), 1));
        }
        for (int line = 0; line <= 5; ++line) {
            const auto x = area.left() + area.width() * line / 5.0;
            painter.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
            const auto seconds = -timeWindowSeconds_ + timeWindowSeconds_ * line / 5.0;
            painter.setPen(QColor("#8090a4"));
            painter.drawText(QRectF(x - 35, area.bottom() + 5, 70, 20), Qt::AlignCenter,
                             QString::number(seconds, 'g', 3) + QStringLiteral(" s"));
            painter.setPen(QPen(QColor("#263244"), 1));
        }

        painter.save();
        painter.setClipRect(area);
        if (selection_) {
            const auto selectedStart = std::max(selection_->first, start);
            const auto selectedEnd = std::min(selection_->second, end);
            if (selectedStart <= selectedEnd) {
                const auto left = xForTimestamp(selectedStart, *range, area);
                const auto right = xForTimestamp(selectedEnd, *range, area);
                painter.fillRect(QRectF(QPointF(left, area.top()),
                                        QPointF(right, area.bottom())),
                                 QColor(65, 145, 230, 48));
                painter.setPen(QPen(QColor("#66b3ff"), 1.2, Qt::DashLine));
                painter.drawLine(QPointF(left, area.top()), QPointF(left, area.bottom()));
                painter.drawLine(QPointF(right, area.top()), QPointF(right, area.bottom()));
            }
        }
        for (std::size_t fieldIndex = 0; fieldIndex < series.size(); ++fieldIndex) {
            const auto& points = series[fieldIndex];
            if (points.empty()) continue;
            QPainterPath path;
            bool started = false;
            for (const auto& point : points) {
                const auto xRatio = static_cast<double>(point.timestamp - start) /
                                    static_cast<double>(windowNs);
                const auto yRatio = (point.value - minimum) / (maximum - minimum);
                const QPointF position(area.left() + xRatio * area.width(),
                                       area.bottom() - yRatio * area.height());
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
        for (const auto& event : events_) {
            if (event.timestamp < start || event.timestamp > end) continue;
            const auto x = xForTimestamp(event.timestamp, *range, area);
            const auto color = event.category == QStringLiteral("alert")
                                   ? QColor(QStringLiteral("#ff9f43"))
                                   : QColor(QStringLiteral("#f7e36d"));
            painter.setPen(QPen(color, 1.4, Qt::DashLine));
            painter.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
            painter.setPen(color);
            const auto label = painter.fontMetrics().elidedText(
                event.message, Qt::ElideRight, 180);
            painter.drawText(QPointF(x + 4, area.top() + 14), label);
        }
        painter.restore();

        qreal legendX = area.left();
        for (int index = 0; index < fields_.size(); ++index) {
            painter.setPen(kColors[static_cast<std::size_t>(index) % kColors.size()]);
            const auto label = fields_[index];
            painter.drawText(QPointF(legendX, 14), label);
            legendX += painter.fontMetrics().horizontalAdvance(label) + 20;
        }
        if (historicalEnd_) {
            painter.setPen(QColor("#ffbd69"));
            painter.drawText(QRectF(area.right() - 90, 1, 90, 18),
                             Qt::AlignRight | Qt::AlignVCenter, tr("历史视图"));
        }
    }

    void wheelEvent(QWheelEvent* event) override {
        const auto factor = event->angleDelta().y() > 0 ? 0.8 : 1.25;
        const auto seconds = std::clamp(
            static_cast<int>(std::lround(timeWindowSeconds_ * factor)), 1, 600);
        setTimeWindow(seconds);
        if (timeWindowChanged_) timeWindowChanged_(seconds);
        event->accept();
    }

    void mousePressEvent(QMouseEvent* event) override {
        const auto range = visibleRange();
        const auto area = plotRect();
        if (event->button() != Qt::LeftButton || !range ||
            !area.contains(event->position())) {
            QWidget::mousePressEvent(event);
            return;
        }
        dragging_ = true;
        dragMoved_ = false;
        dragStartPosition_ = event->position();
        dragRange_ = *range;
        if (selectionMode_) {
            const auto timestamp = timestampForX(event->position().x(), *range, area);
            selection_ = Range{timestamp, timestamp};
            if (selectionChanged_) selectionChanged_(selection_);
        } else {
            setCursor(Qt::ClosedHandCursor);
        }
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && dragging_) {
            dragging_ = false;
            setCursor(selectionMode_ ? Qt::CrossCursor : Qt::OpenHandCursor);
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        const auto area = plotRect();
        if (dragging_) {
            const auto delta = event->position().x() - dragStartPosition_.x();
            dragMoved_ = dragMoved_ || std::abs(delta) >= 2.0;
            if (selectionMode_) {
                const auto timestamp = timestampForX(event->position().x(), dragRange_, area);
                const auto anchor = timestampForX(dragStartPosition_.x(), dragRange_, area);
                selection_ = Range{std::min(anchor, timestamp), std::max(anchor, timestamp)};
                if (selectionChanged_) selectionChanged_(selection_);
                update();
            } else if (dragMoved_) {
                const auto shift = static_cast<lab::core::Timestamp>(
                    delta / area.width() * static_cast<double>(windowNanoseconds()));
                historicalEnd_ = dragRange_.second - shift;
                clampHistoricalEnd();
                if (historyEntered_) historyEntered_();
                update();
            }
            event->accept();
            return;
        }
        const auto range = visibleRange();
        if (!range || !area.contains(event->position()) || fields_.isEmpty()) return;
        const auto cursorTime = timestampForX(event->position().x(), *range, area);
        QStringList lines;
        lines << tr("相对视窗末端 %1 s")
                     .arg(lab::core::secondsBetween(range->second, cursorTime), 0, 'f', 3);
        for (const auto& field : fields_) {
            const auto points = store_->snapshot(
                field.toStdString(), range->first, range->second);
            if (points.empty()) continue;
            auto iterator = std::lower_bound(
                points.begin(), points.end(), cursorTime,
                [](const auto& point, auto timestamp) { return point.timestamp < timestamp; });
            if (iterator == points.end()) {
                iterator = std::prev(points.end());
            } else if (iterator != points.begin()) {
                const auto previous = std::prev(iterator);
                if (cursorTime - previous->timestamp <= iterator->timestamp - cursorTime) {
                    iterator = previous;
                }
            }
            lines << QStringLiteral("%1 = %2").arg(field).arg(iterator->value, 0, 'g', 8);
        }
        const auto tolerance = static_cast<lab::core::Timestamp>(
            8.0 / area.width() * static_cast<double>(windowNanoseconds()));
        for (const auto& marker : events_) {
            const auto distance = marker.timestamp >= cursorTime
                                      ? marker.timestamp - cursorTime
                                      : cursorTime - marker.timestamp;
            if (distance <= tolerance) {
                lines << QStringLiteral("%1: %2")
                             .arg(marker.category == QStringLiteral("alert")
                                      ? tr("告警")
                                      : tr("Marker"),
                                  marker.message);
            }
        }
        QToolTip::showText(event->globalPosition().toPoint(), lines.join('\n'), this);
    }

private:
    [[nodiscard]] QRectF plotRect() const {
        return QRectF(64, 20, width() - 86, height() - 62);
    }

    [[nodiscard]] lab::core::Timestamp windowNanoseconds() const {
        return static_cast<lab::core::Timestamp>(timeWindowSeconds_ * 1e9);
    }

    [[nodiscard]] std::optional<Range> dataBounds() const {
        std::optional<Range> result;
        for (const auto& field : fields_) {
            const auto bounds = store_->bounds(field.toStdString());
            if (!bounds) continue;
            if (!result) {
                result = Range{bounds->earliest, bounds->latest};
            } else {
                result->first = std::min(result->first, bounds->earliest);
                result->second = std::max(result->second, bounds->latest);
            }
        }
        for (const auto& event : events_) {
            if (!result) {
                result = Range{event.timestamp, event.timestamp};
            } else {
                result->first = std::min(result->first, event.timestamp);
                result->second = std::max(result->second, event.timestamp);
            }
        }
        return result;
    }

    [[nodiscard]] lab::core::Timestamp newestTimestamp() const {
        const auto bounds = dataBounds();
        return bounds ? bounds->second : 0;
    }

    void clampHistoricalEnd() {
        if (!historicalEnd_) return;
        const auto bounds = dataBounds();
        if (!bounds) {
            historicalEnd_.reset();
            return;
        }
        const auto span = bounds->second - bounds->first;
        const auto minimumEnd = span <= windowNanoseconds()
                                    ? bounds->second
                                    : bounds->first + windowNanoseconds();
        historicalEnd_ = std::clamp(*historicalEnd_, minimumEnd, bounds->second);
    }

    [[nodiscard]] static lab::core::Timestamp timestampForX(
        qreal x, const Range& range, const QRectF& area) {
        const auto ratio = std::clamp((x - area.left()) / area.width(), 0.0, 1.0);
        return range.first + static_cast<lab::core::Timestamp>(
                                 ratio * static_cast<double>(range.second - range.first));
    }

    [[nodiscard]] static qreal xForTimestamp(
        lab::core::Timestamp timestamp, const Range& range, const QRectF& area) {
        const auto ratio = static_cast<double>(timestamp - range.first) /
                           static_cast<double>(range.second - range.first);
        return area.left() + ratio * area.width();
    }

    const lab::core::TimeSeriesStore* store_;
    QStringList fields_;
    double timeWindowSeconds_{10.0};
    bool paused_{};
    bool automaticY_{true};
    bool selectionMode_{};
    bool dragging_{};
    bool dragMoved_{};
    double manualMinimum_{-1.0};
    double manualMaximum_{1.0};
    lab::core::Timestamp frozenEnd_{};
    std::optional<lab::core::Timestamp> historicalEnd_;
    std::optional<Range> selection_;
    QPointF dragStartPosition_;
    Range dragRange_{};
    std::vector<VisualTimelineEvent> events_;
    SelectionCallback selectionChanged_;
    TimeWindowCallback timeWindowChanged_;
    HistoryCallback historyEntered_;
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
    paused_->setObjectName(QStringLiteral("plotPaused"));
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

    selectionMode_ = new QToolButton(this);
    selectionMode_->setObjectName(QStringLiteral("plotSelectionMode"));
    selectionMode_->setText(tr("框选区间"));
    selectionMode_->setCheckable(true);
    selectionMode_->setToolTip(tr("启用后在曲线上左键拖动选择导出区间"));
    returnLive_ = new QPushButton(tr("回到实时"), this);
    returnLive_->setObjectName(QStringLiteral("plotReturnLive"));
    clearSelection_ = new QPushButton(tr("清除选择"), this);
    clearSelection_->setObjectName(QStringLiteral("plotClearSelection"));
    clearSelection_->setEnabled(false);
    exportSelection_ = new QPushButton(tr("导出所选 CSV"), this);
    exportSelection_->setObjectName(QStringLiteral("plotExportSelection"));
    exportSelection_->setEnabled(false);
    selectionStatus_ = new QLabel(tr("未选择区间；左键拖动浏览历史，滚轮缩放"), this);
    selectionStatus_->setObjectName(QStringLiteral("plotSelectionStatus"));

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

    auto* historyControls = new QHBoxLayout;
    historyControls->addWidget(returnLive_);
    historyControls->addWidget(selectionMode_);
    historyControls->addWidget(clearSelection_);
    historyControls->addWidget(exportSelection_);
    historyControls->addStretch(1);

    auto* side = new QWidget(this);
    auto* sideLayout = new QVBoxLayout(side);
    sideLayout->addLayout(controls);
    sideLayout->addWidget(new QLabel(tr("显示字段"), side));
    sideLayout->addWidget(visibleFields_);
    sideLayout->addStretch(1);

    canvas_ = new PlotCanvas(store_, this);
    canvas_->setObjectName(QStringLiteral("plotCanvas"));
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
    layout->addLayout(historyControls);
    layout->addWidget(selectionStatus_);
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
    connect(selectionMode_, &QToolButton::toggled,
            canvas_, &PlotCanvas::setSelectionMode);
    connect(returnLive_, &QPushButton::clicked, this, &PlotWidget::returnToLive);
    connect(clearSelection_, &QPushButton::clicked,
            this, &PlotWidget::clearRangeSelection);
    connect(exportSelection_, &QPushButton::clicked,
            this, &PlotWidget::exportSelectedRange);
    connect(refreshTimer_, &QTimer::timeout, this, &PlotWidget::updatePlotAndStatistics);
    canvas_->setSelectionCallback([this](const auto& range) {
        const auto selected = range && range->first < range->second;
        clearSelection_->setEnabled(range.has_value());
        exportSelection_->setEnabled(selected && !visibleFieldNames().isEmpty());
        if (!range) {
            selectionStatus_->setText(
                tr("未选择区间；左键拖动浏览历史，滚轮缩放"));
            return;
        }
        const auto duration = lab::core::secondsBetween(range->first, range->second);
        selectionStatus_->setText(
            tr("已选择 %1 s（%2 至 %3 ns）")
                .arg(duration, 0, 'f', 6)
                .arg(static_cast<qlonglong>(range->first))
                .arg(static_cast<qlonglong>(range->second)));
    });
    canvas_->setTimeWindowCallback([this](int seconds) {
        const QSignalBlocker blocker(timeWindow_);
        timeWindow_->setValue(seconds);
        updatePlotAndStatistics();
    });
    canvas_->setHistoryCallback([this] {
        if (!paused_->isChecked()) paused_->setChecked(true);
    });
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

void PlotWidget::useExternalFields(const QStringList& fields) {
    if (fields.isEmpty()) {
        return;
    }
    rebuildVisibleFields(fields, true);
}

void PlotWidget::setTimelineEvents(const QVariantList& events) {
    canvas_->setTimelineEvents(events);
}

QStringList PlotWidget::fieldNames() const {
    return configuredFields();
}

bool PlotWidget::exportRangeCsv(
    const QString& filePath,
    lab::core::Timestamp start,
    lab::core::Timestamp end,
    const QStringList& fields,
    QString* error,
    quint64* rowCount) const {
    if (error) error->clear();
    if (rowCount) *rowCount = 0;
    if (filePath.trimmed().isEmpty()) {
        if (error) *error = tr("导出路径为空");
        return false;
    }

    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = tr("无法创建导出文件：%1").arg(file.errorString());
        return false;
    }

    lab::core::TimeSeriesCsvExportOptions options;
    options.start = start;
    options.end = end;
    options.fields.reserve(static_cast<std::size_t>(fields.size()));
    for (const auto& field : fields) {
        options.fields.push_back(field.toStdString());
    }
    lab::core::TimeSeriesCsvExportSummary summary;
    std::string exportError;
    const auto exported = lab::core::exportTimeSeriesCsv(
        *store_, options,
        [&file](std::string_view text) {
            const auto size = static_cast<qint64>(text.size());
            return file.write(text.data(), size) == size;
        },
        &summary, &exportError);
    if (!exported) {
        file.cancelWriting();
        if (error) *error = QString::fromStdString(exportError);
        return false;
    }
    if (!file.commit()) {
        if (error) *error = tr("无法完成原子写入：%1").arg(file.errorString());
        return false;
    }
    if (rowCount) *rowCount = static_cast<quint64>(summary.rowCount);
    return true;
}

void PlotWidget::applyFields() {
    rebuildFieldControls(true);
}

void PlotWidget::rebuildFieldControls(bool notifyParser) {
    const auto names = configuredFields();
    rebuildVisibleFields(names, false);
    if (notifyParser) {
        emit fieldsChanged(names);
    }
}

void PlotWidget::rebuildVisibleFields(const QStringList& fields, bool preserveChecks) {
    QSet<QString> unchecked;
    if (preserveChecks) {
        for (int row = 0; row < visibleFields_->count(); ++row) {
            const auto* item = visibleFields_->item(row);
            if (item->checkState() != Qt::Checked) {
                unchecked.insert(item->text());
            }
        }
    }
    visibleFields_->blockSignals(true);
    visibleFields_->clear();
    for (const auto& name : fields) {
        auto* item = new QListWidgetItem(name, visibleFields_);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(unchecked.contains(name) ? Qt::Unchecked : Qt::Checked);
    }
    visibleFields_->blockSignals(false);
    updateSelectedFields();
}

void PlotWidget::updatePlotAndStatistics() {
    if (!paused_->isChecked()) {
        canvas_->update();
    }
    const auto fields = visibleFieldNames();
    statistics_->setRowCount(fields.size());
    const auto visibleRange = canvas_->visibleRange();
    for (int row = 0; row < fields.size(); ++row) {
        statistics_->setItem(row, 0, new QTableWidgetItem(fields[row]));
        const auto stats = visibleRange
                               ? store_->statistics(fields[row].toStdString(),
                                                    visibleRange->first,
                                                    visibleRange->second)
                               : std::nullopt;
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
    const auto selected = visibleFieldNames();
    canvas_->setFields(selected);
    const auto range = canvas_->selection();
    exportSelection_->setEnabled(range && range->first < range->second &&
                                 !selected.isEmpty());
    updatePlotAndStatistics();
}

void PlotWidget::updateRanges() {
    yMinimum_->setEnabled(!autoY_->isChecked());
    yMaximum_->setEnabled(!autoY_->isChecked());
    canvas_->setTimeWindow(timeWindow_->value());
    canvas_->setPaused(paused_->isChecked());
    canvas_->setYRange(autoY_->isChecked(), yMinimum_->value(), yMaximum_->value());
    updatePlotAndStatistics();
}

void PlotWidget::returnToLive() {
    if (paused_->isChecked()) {
        paused_->setChecked(false);
    } else {
        canvas_->resetToLive();
        updatePlotAndStatistics();
    }
}

void PlotWidget::clearRangeSelection() {
    canvas_->clearSelection();
}

void PlotWidget::exportSelectedRange() {
    const auto range = canvas_->selection();
    const auto fields = visibleFieldNames();
    if (!range || range->first >= range->second || fields.isEmpty()) return;

    const auto suggested = QStringLiteral("lab_debugger_%1.csv")
                               .arg(QDateTime::currentDateTime().toString(
                                   QStringLiteral("yyyyMMdd_HHmmss")));
    auto path = QFileDialog::getSaveFileName(
        this, tr("导出所选曲线"), suggested, tr("CSV 文件 (*.csv)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".csv"), Qt::CaseInsensitive)) {
        path += QStringLiteral(".csv");
    }
    QString error;
    quint64 rows = 0;
    if (!exportRangeCsv(path, range->first, range->second, fields, &error, &rows)) {
        QMessageBox::critical(this, tr("导出失败"), error);
        return;
    }
    QMessageBox::information(
        this, tr("导出完成"),
        tr("已导出 %1 行样本。\n%2").arg(rows).arg(path));
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

QStringList PlotWidget::visibleFieldNames() const {
    QStringList result;
    result.reserve(visibleFields_->count());
    for (int row = 0; row < visibleFields_->count(); ++row) {
        const auto* item = visibleFields_->item(row);
        if (item->checkState() == Qt::Checked) {
            result.push_back(item->text());
        }
    }
    return result;
}

}  // namespace lab::ui

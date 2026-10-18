#include "ui/main_window.hpp"
#include "ui/derived_fields_widget.hpp"
#include "ui/alerts_widget.hpp"
#include "ui/plot_widget.hpp"
#include "ui/protocol_widget.hpp"
#include "ui/send_panel.hpp"
#include "ui/serial_panel.hpp"
#include "ui/network_panel.hpp"
#include "ui/time_alignment_widget.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QFile>
#include <QMouseEvent>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QWheelEvent>

#include <iostream>

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("LabDebugger"));
    QCoreApplication::setApplicationName(QStringLiteral("Lab Debugger GUI Test"));

    int result = -1;
    {
        lab::ui::SendPanel sendPanel;
        sendPanel.setTarget(1);
        if (sendPanel.target() != 1) {
            std::cerr << "GUI smoke test failed: network send target was not selected\n";
            return 1;
        }
        QVariantMap serialSource;
        serialSource.insert(QStringLiteral("id"), QStringLiteral("serial:COM5"));
        serialSource.insert(QStringLiteral("type"), QStringLiteral("serial"));
        serialSource.insert(QStringLiteral("selected"), true);
        serialSource.insert(QStringLiteral("state"),
                            static_cast<int>(lab::core::SourceState::Open));
        serialSource.insert(QStringLiteral("open"), true);
        serialSource.insert(QStringLiteral("port"), QStringLiteral("COM5"));
        serialSource.insert(QStringLiteral("baud_rate"), 115200);
        serialSource.insert(QStringLiteral("data_bits"), 8);
        serialSource.insert(QStringLiteral("stop_bits"), 0);
        serialSource.insert(QStringLiteral("parity"), 0);
        serialSource.insert(QStringLiteral("flow_control"), 0);
        QVariantMap networkSource;
        networkSource.insert(QStringLiteral("id"),
                             QStringLiteral("udp:127.0.0.1:9000"));
        networkSource.insert(QStringLiteral("type"), QStringLiteral("network"));
        networkSource.insert(QStringLiteral("state"),
                             static_cast<int>(lab::core::SourceState::Open));
        networkSource.insert(QStringLiteral("open"), true);
        networkSource.insert(QStringLiteral("mode"),
                             static_cast<int>(lab::adapters::network::NetworkMode::Udp));
        networkSource.insert(QStringLiteral("remote_host"),
                             QStringLiteral("127.0.0.1"));
        networkSource.insert(QStringLiteral("remote_port"), 9001);
        networkSource.insert(QStringLiteral("bind_address"),
                             QStringLiteral("127.0.0.1"));
        networkSource.insert(QStringLiteral("local_port"), 9000);
        const QVariantList localSources{serialSource, networkSource};
        sendPanel.setSources(localSources);
        sendPanel.setTargetSource(QStringLiteral("udp:127.0.0.1:9000"));
        if (sendPanel.targetSourceId() != QStringLiteral("udp:127.0.0.1:9000") ||
            sendPanel.target() != 1) {
            std::cerr << "GUI smoke test failed: exact dynamic send target was not selected\n";
            return 1;
        }
        networkSource.insert(QStringLiteral("state"),
                             static_cast<int>(lab::core::SourceState::Closed));
        networkSource.insert(QStringLiteral("open"), false);
        sendPanel.setSources({serialSource, networkSource});
        if (sendPanel.targetSourceId() != QStringLiteral("udp:127.0.0.1:9000")) {
            std::cerr << "GUI smoke test failed: closed exact send target was lost\n";
            return 1;
        }
        networkSource.insert(QStringLiteral("state"),
                             static_cast<int>(lab::core::SourceState::Open));
        networkSource.insert(QStringLiteral("open"), true);

        lab::ui::SerialPanel serialPanel;
        serialPanel.setSources(localSources);
        lab::ui::NetworkPanel networkPanel;
        networkPanel.setSources(localSources);
        auto* serialList = serialPanel.findChild<QListWidget*>(
            QStringLiteral("serialSourceList"));
        auto* networkList = networkPanel.findChild<QListWidget*>(
            QStringLiteral("networkSourceList"));
        if (!serialList || serialList->count() != 1 ||
            serialPanel.selectedSourceId() != QStringLiteral("serial:COM5") ||
            !networkList || networkList->count() != 1 ||
            networkPanel.selectedSourceId() !=
                QStringLiteral("udp:127.0.0.1:9000")) {
            std::cerr << "GUI smoke test failed: dynamic source lists are not usable\n";
            return 1;
        }
        auto secondSerial = serialSource;
        secondSerial.insert(QStringLiteral("id"), QStringLiteral("serial:COM6"));
        secondSerial.insert(QStringLiteral("selected"), false);
        secondSerial.insert(QStringLiteral("port"), QStringLiteral("COM6"));
        const QVariantList expandedSources{serialSource, secondSerial, networkSource};
        serialPanel.setSources(expandedSources);
        serialList->setCurrentRow(1);
        serialPanel.setSources(expandedSources);
        if (serialPanel.selectedSourceId() != QStringLiteral("serial:COM6")) {
            std::cerr << "GUI smoke test failed: user-selected dynamic source was reset\n";
            return 1;
        }

        lab::core::TimeSeriesStore store(100);
        lab::ui::PlotWidget plot(&store);
        const auto csvFields = plot.fieldNames();
        plot.useExternalFields(
            {QStringLiteral("serial:COM5.speed"),
             QStringLiteral("udp:127.0.0.1:9000.speed")});
        const auto* visibleFields = plot.findChild<QListWidget*>();
        if (!visibleFields || visibleFields->count() != 2 ||
            visibleFields->item(0)->text() != QStringLiteral("serial:COM5.speed") ||
            plot.fieldNames() != csvFields) {
            std::cerr << "GUI smoke test failed: source-qualified plot fields changed CSV parsing\n";
            return 1;
        }
        auto* selectionMode = plot.findChild<QToolButton*>(
            QStringLiteral("plotSelectionMode"));
        auto* returnLive = plot.findChild<QPushButton*>(
            QStringLiteral("plotReturnLive"));
        auto* exportSelection = plot.findChild<QPushButton*>(
            QStringLiteral("plotExportSelection"));
        auto* clearSelection = plot.findChild<QPushButton*>(
            QStringLiteral("plotClearSelection"));
        auto* plotCanvas = plot.findChild<QWidget*>(QStringLiteral("plotCanvas"));
        auto* plotPaused = plot.findChild<QCheckBox*>(QStringLiteral("plotPaused"));
        auto* plotTimeWindow = plot.findChild<QSpinBox*>();
        auto* plotStatistics = plot.findChild<QTableWidget*>(
            QStringLiteral("plotStatistics"));
        auto* statisticsScope = plot.findChild<QLabel*>(
            QStringLiteral("plotStatisticsScope"));
        if (!selectionMode || !returnLive || !exportSelection ||
            !clearSelection || !plotCanvas || !plotPaused || !plotTimeWindow ||
            !plotStatistics || !statisticsScope || !selectionMode->isCheckable() ||
            exportSelection->isEnabled()) {
            std::cerr << "GUI smoke test failed: plot history controls are not usable\n";
            return 1;
        }
        store.append({1'000'000'000, "serial:COM5", "serial:COM5.speed",
                      1.25, "m/s", 0});
        store.append({2'000'000'000, "udp", "udp:127.0.0.1:9000.speed",
                      2.5, "m/s", 0});
        store.append({14'000'000'000, "serial:COM5", "serial:COM5.speed",
                      14.0, "m/s", 1});
        store.append({15'000'000'000, "serial:COM5", "serial:COM5.speed",
                      15.0, "m/s", 2});
        store.append({20'000'000'000, "serial:COM5", "serial:COM5.speed",
                      1000.0, "m/s", 3});
        plot.resize(900, 600);
        plot.show();
        QCoreApplication::processEvents();
        const auto sendPlotMouse = [&](QEvent::Type type, const QPointF& local,
                                       Qt::MouseButton button,
                                       Qt::MouseButtons buttons) {
            const auto global = plotCanvas->mapToGlobal(local.toPoint());
            QMouseEvent event(type, local, QPointF(global), button, buttons,
                              Qt::NoModifier);
            QApplication::sendEvent(plotCanvas, &event);
        };
        const auto wheelPosition = QPointF(240, 120);
        QWheelEvent wheelEvent(
            wheelPosition, QPointF(plotCanvas->mapToGlobal(wheelPosition.toPoint())),
            QPoint{}, QPoint{0, 120}, Qt::NoButton, Qt::NoModifier,
            Qt::ScrollUpdate, false);
        QApplication::sendEvent(plotCanvas, &wheelEvent);
        if (plotTimeWindow->value() != 8) {
            std::cerr << "GUI smoke test failed: plot wheel did not synchronize the time window\n";
            return 1;
        }
        selectionMode->setChecked(true);
        sendPlotMouse(QEvent::MouseButtonPress, QPointF(140, 120),
                      Qt::LeftButton, Qt::LeftButton);
        sendPlotMouse(QEvent::MouseButtonRelease, QPointF(140, 120),
                      Qt::LeftButton, Qt::NoButton);
        if (exportSelection->isEnabled()) {
            std::cerr << "GUI smoke test failed: zero-width selection enabled export\n";
            return 1;
        }
        sendPlotMouse(QEvent::MouseButtonPress, QPointF(140, 120),
                      Qt::LeftButton, Qt::LeftButton);
        sendPlotMouse(QEvent::MouseMove, QPointF(340, 120),
                      Qt::NoButton, Qt::LeftButton);
        sendPlotMouse(QEvent::MouseButtonRelease, QPointF(340, 120),
                      Qt::LeftButton, Qt::NoButton);
        if (!exportSelection->isEnabled()) {
            std::cerr << "GUI smoke test failed: plot drag did not create an export range\n";
            return 1;
        }
        visibleFields->item(0)->setCheckState(Qt::Unchecked);
        visibleFields->item(1)->setCheckState(Qt::Unchecked);
        if (exportSelection->isEnabled()) {
            std::cerr << "GUI smoke test failed: empty field selection enabled export\n";
            return 1;
        }
        visibleFields->item(0)->setCheckState(Qt::Checked);
        if (!exportSelection->isEnabled()) {
            std::cerr << "GUI smoke test failed: valid selected field did not enable export\n";
            return 1;
        }
        const auto* selectedCurrent = plotStatistics->item(0, 1);
        const auto* selectedCount = plotStatistics->item(0, 5);
        if (!statisticsScope->text().contains(QStringLiteral("框选区间")) ||
            !selectedCurrent || selectedCurrent->text() != QStringLiteral("15") ||
            !selectedCount || selectedCount->text() != QStringLiteral("2")) {
            std::cerr << "GUI smoke test failed: selected range statistics are incorrect\n";
            return 1;
        }
        clearSelection->click();
        if (exportSelection->isEnabled() || clearSelection->isEnabled() ||
            !statisticsScope->text().contains(QStringLiteral("当前可见窗口"))) {
            std::cerr << "GUI smoke test failed: clear selection did not reset controls\n";
            return 1;
        }
        selectionMode->setChecked(false);
        sendPlotMouse(QEvent::MouseButtonPress, QPointF(340, 120),
                      Qt::LeftButton, Qt::LeftButton);
        sendPlotMouse(QEvent::MouseMove, QPointF(1340, 120),
                      Qt::NoButton, Qt::LeftButton);
        sendPlotMouse(QEvent::MouseButtonRelease, QPointF(1340, 120),
                      Qt::LeftButton, Qt::NoButton);
        if (!plotPaused->isChecked()) {
            std::cerr << "GUI smoke test failed: historical drag did not freeze the view\n";
            return 1;
        }
        store.append({30'000'000'000, "serial:COM5", "serial:COM5.speed",
                      2000.0, "m/s", 2});
        QCoreApplication::processEvents();
        const auto backgroundSamples =
            store.snapshot("serial:COM5.speed", 30'000'000'000, 30'000'000'000);
        const auto* historicalCurrent = plotStatistics->item(0, 1);
        if (backgroundSamples.size() != 1 || !historicalCurrent ||
            historicalCurrent->text() != QStringLiteral("1.25")) {
            std::cerr << "GUI smoke test failed: paused history stopped acquisition or used live statistics\n";
            return 1;
        }
        returnLive->click();
        if (plotPaused->isChecked()) {
            std::cerr << "GUI smoke test failed: return-to-live did not resume tracking\n";
            return 1;
        }
        QTemporaryDir plotExportDirectory(
            QDir::current().filePath(QStringLiteral("plot-export-XXXXXX")));
        const auto plotExportPath =
            plotExportDirectory.filePath(QStringLiteral("selected.csv"));
        QString plotExportError;
        quint64 plotExportRows = 0;
        if (!plotExportDirectory.isValid() ||
            !plot.exportRangeCsv(
                plotExportPath, 1'000'000'000, 2'000'000'000,
                {QStringLiteral("serial:COM5.speed")},
                &plotExportError, &plotExportRows) ||
            plotExportRows != 1) {
            std::cerr << "GUI smoke test failed: selected plot CSV export failed: "
                      << plotExportError.toStdString() << '\n';
            return 1;
        }
        QFile plotExportFile(plotExportPath);
        if (!plotExportFile.open(QIODevice::ReadOnly) ||
            !plotExportFile.readAll().contains("timestamp_ns,relative_seconds,field,value")) {
            std::cerr << "GUI smoke test failed: selected plot CSV file is invalid\n";
            return 1;
        }

        lab::ui::DerivedFieldsWidget derivedFields;
        QVariantMap power;
        power.insert(QStringLiteral("name"), QStringLiteral("power"));
        power.insert(QStringLiteral("expression"),
                     QStringLiteral("voltage * current"));
        power.insert(QStringLiteral("unit"), QStringLiteral("W"));
        derivedFields.setDefinitions({power});
        if (derivedFields.definitions().size() != 1 ||
            derivedFields.definitions().front().toMap()
                    .value(QStringLiteral("expression")).toString() !=
                QStringLiteral("voltage * current")) {
            std::cerr << "GUI smoke test failed: derived field table does not preserve input\n";
            return 1;
        }

        lab::ui::AlertsWidget alerts;
        QVariantMap rule;
        rule.insert(QStringLiteral("name"), QStringLiteral("overheat"));
        rule.insert(QStringLiteral("field"), QStringLiteral("temperature"));
        rule.insert(QStringLiteral("comparison"), QStringLiteral("above"));
        rule.insert(QStringLiteral("threshold"), 80.0);
        rule.insert(QStringLiteral("hysteresis"), 5.0);
        rule.insert(QStringLiteral("duration_ms"), 250);
        rule.insert(QStringLiteral("message"), QStringLiteral("check cooling"));
        alerts.setDefinitions({rule});
        if (alerts.definitions().size() != 1 ||
            alerts.definitions().front().toMap()
                    .value(QStringLiteral("hysteresis")).toDouble() != 5.0 ||
            alerts.definitions().front().toMap()
                    .value(QStringLiteral("duration_ms")).toInt() != 250) {
            std::cerr << "GUI smoke test failed: alert rule table does not preserve input\n";
            return 1;
        }
        QVariantMap healthRule;
        healthRule.insert(QStringLiteral("name"), QStringLiteral("udp_silent"));
        healthRule.insert(QStringLiteral("source_id"),
                          QStringLiteral("udp:127.0.0.1:9000"));
        healthRule.insert(QStringLiteral("kind"), QStringLiteral("inactivity"));
        healthRule.insert(QStringLiteral("error_count"), 1);
        healthRule.insert(QStringLiteral("window_ms"), 1500);
        healthRule.insert(QStringLiteral("message"), QStringLiteral("check link"));
        alerts.setHealthDefinitions({healthRule});
        if (alerts.healthDefinitions().size() != 1 ||
            alerts.healthDefinitions().front().toMap()
                    .value(QStringLiteral("source_id")).toString() !=
                QStringLiteral("udp:127.0.0.1:9000") ||
            alerts.healthDefinitions().front().toMap()
                    .value(QStringLiteral("window_ms")).toInt() != 1500) {
            std::cerr << "GUI smoke test failed: health alert table does not preserve input\n";
            return 1;
        }

        lab::ui::TimeAlignmentWidget alignment;
        QVariantMap alignmentRule;
        alignmentRule.insert(QStringLiteral("source_id"),
                             QStringLiteral("ros-agent:demo"));
        alignmentRule.insert(QStringLiteral("mode"),
                             QStringLiteral("remote_clock"));
        alignment.setDefinitions({alignmentRule});
        QVariantMap alignmentStatus;
        alignmentStatus.insert(QStringLiteral("source_id"),
                               QStringLiteral("ros-agent:demo:/imu"));
        alignmentStatus.insert(QStringLiteral("mode"),
                               QStringLiteral("remote_clock"));
        alignmentStatus.insert(QStringLiteral("sample_count"), 4);
        alignmentStatus.insert(QStringLiteral("fallback_count"), 0);
        alignmentStatus.insert(QStringLiteral("out_of_order_count"), 0);
        alignmentStatus.insert(QStringLiteral("applied_offset_ns"), -2'000'000);
        alignmentStatus.insert(QStringLiteral("latency_ns"), 3'000'000);
        alignmentStatus.insert(QStringLiteral("uncertainty_ns"), 500'000);
        alignment.setStatuses({alignmentStatus});
        const auto alignmentDefinitions = alignment.definitions();
        auto* alignmentStatuses = alignment.findChild<QTableWidget*>(
            QStringLiteral("timeAlignmentStatuses"));
        if (alignmentDefinitions.size() != 1 ||
            alignmentDefinitions.front().toMap()
                    .value(QStringLiteral("mode")).toString() !=
                QStringLiteral("remote_clock") ||
            !alignmentStatuses || alignmentStatuses->rowCount() != 1) {
            std::cerr << "GUI smoke test failed: time alignment UI does not preserve state\n";
            return 1;
        }

        lab::ui::ProtocolWidget protocol;
        QVariantMap parserSource;
        parserSource.insert(QStringLiteral("id"),
                            QStringLiteral("udp:127.0.0.1:9000"));
        parserSource.insert(QStringLiteral("label"),
                            QStringLiteral("udp:127.0.0.1:9000"));
        parserSource.insert(QStringLiteral("overridden"), true);
        parserSource.insert(QStringLiteral("mode"), QStringLiteral("csv"));
        protocol.setParserSources({parserSource});
        auto* sourceSelector = protocol.findChild<QComboBox*>(
            QStringLiteral("parserSourceSelector"));
        auto* sourceFields = protocol.findChild<QLineEdit*>(
            QStringLiteral("sourceCsvFields"));
        if (!sourceSelector || sourceSelector->count() != 2 || !sourceFields) {
            std::cerr << "GUI smoke test failed: source parser controls are missing\n";
            return 1;
        }
        sourceSelector->setCurrentIndex(1);
        protocol.showParserConfigured(QStringLiteral("udp:127.0.0.1:9000"),
                                      QStringLiteral("csv"), {},
                                      {QStringLiteral("temperature"),
                                       QStringLiteral("voltage")});
        if (sourceFields->text() != QStringLiteral("temperature,voltage")) {
            std::cerr << "GUI smoke test failed: source parser fields are not restored\n";
            return 1;
        }

        lab::ui::MainWindow window;
        window.show();
        if (!window.isVisible()) {
            std::cerr << "GUI smoke test failed: main window is not visible\n";
            return 1;
        }
        if (!window.findChild<lab::ui::DerivedFieldsWidget*>(
                QStringLiteral("derivedFieldsWidget"))) {
            std::cerr << "GUI smoke test failed: derived fields tab is missing\n";
            return 1;
        }
        if (!window.findChild<lab::ui::AlertsWidget*>(
                QStringLiteral("alertsWidget"))) {
            std::cerr << "GUI smoke test failed: marker and alerts tab is missing\n";
            return 1;
        }
        if (!window.findChild<lab::ui::TimeAlignmentWidget*>(
                QStringLiteral("timeAlignmentWidget"))) {
            std::cerr << "GUI smoke test failed: time alignment tab is missing\n";
            return 1;
        }
        QTimer::singleShot(150, &application, [] {
            QCoreApplication::exit(0);
        });
        result = application.exec();
    }

    if (result != 0) {
        std::cerr << "GUI smoke test failed: event loop returned " << result << '\n';
        return 1;
    }
    std::cout << "Lab Debugger GUI smoke test passed.\n";
    return 0;
}

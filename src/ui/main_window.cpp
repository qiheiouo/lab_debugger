#include "ui/main_window.hpp"

#include "ui/derived_fields_widget.hpp"
#include "ui/alerts_widget.hpp"
#include "ui/network_panel.hpp"
#include "ui/remote_agent_panel.hpp"
#include "ui/plot_widget.hpp"
#include "ui/protocol_widget.hpp"
#include "ui/replay_widget.hpp"
#include "ui/send_panel.hpp"
#include "ui/serial_panel.hpp"
#include "ui/terminal_widget.hpp"

#include <QAction>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QLabel>
#include <QInputDialog>
#include <QMessageBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>
#include <QVBoxLayout>

namespace lab::ui {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(tr("Lab Debugger / 实验室调试助手"));
    resize(1280, 820);
    setMinimumSize(900, 600);

    auto* toolbar = addToolBar(tr("主工具栏"));
    toolbar->setMovable(false);
    recordAction_ = toolbar->addAction(tr("开始 Session 记录"));
    recordAction_->setCheckable(true);
    auto* markerAction = toolbar->addAction(tr("添加 Marker"));
    toolbar->addSeparator();
    auto* architectureAction = toolbar->addAction(tr("架构状态"));

    serialPanel_ = new SerialPanel(this);
    networkPanel_ = new NetworkPanel(this);
    remoteAgentPanel_ = new RemoteAgentPanel(this);
    terminal_ = new TerminalWidget(this);
    plot_ = new PlotWidget(&session_.timeSeries(), this);
    derivedFields_ = new DerivedFieldsWidget(this);
    alerts_ = new AlertsWidget(this);
    protocol_ = new ProtocolWidget(this);
    replay_ = new ReplayWidget(this);
    sendPanel_ = new SendPanel(this);

    auto* tabs = new QTabWidget(this);
    tabs->addTab(terminal_, tr("终端"));
    tabs->addTab(plot_, tr("实时曲线"));
    tabs->addTab(derivedFields_, tr("派生变量"));
    tabs->addTab(alerts_, tr("Marker 与告警"));
    tabs->addTab(protocol_, tr("协议解析"));
    tabs->addTab(replay_, tr("Session 回放"));

    auto* right = new QWidget(this);
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->addWidget(tabs, 1);
    rightLayout->addWidget(sendPanel_);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    auto* sources = new QTabWidget(this);
    sources->addTab(serialPanel_, tr("串口"));
    sources->addTab(networkPanel_, tr("网络"));
    sources->addTab(remoteAgentPanel_, tr("ROS Agent"));
    sources->setMinimumWidth(340);
    splitter->addWidget(sources);
    splitter->addWidget(right);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({270, 1010});
    setCentralWidget(splitter);

    traffic_ = new QLabel(tr("RX 0 B  |  TX 0 B"), this);
    parserStatus_ = new QLabel(tr("解析队列 0"), this);
    statusBar()->addPermanentWidget(traffic_);
    statusBar()->addPermanentWidget(parserStatus_);
    statusBar()->showMessage(tr("就绪"));

    connect(serialPanel_, &SerialPanel::connectRequested, this, [this] {
        const auto configuration = serialPanel_->settings();
        if (configuration.portName.empty()) {
            statusBar()->showMessage(tr("请先选择串口"), 3000);
            return;
        }
        sendPanel_->setTarget(0);
        session_.connectSerial(configuration);
    });
    connect(serialPanel_, &SerialPanel::disconnectRequested,
            &session_, &lab::app::SerialSession::disconnectSerial);
    connect(serialPanel_, &SerialPanel::reconnectRequested, this, [this] {
        sendPanel_->setTarget(0);
        session_.reconnectSerial();
    });
    connect(networkPanel_, &NetworkPanel::connectRequested, this, [this] {
        sendPanel_->setTarget(1);
        session_.connectNetwork(networkPanel_->settings());
    });
    connect(networkPanel_, &NetworkPanel::disconnectRequested,
            &session_, &lab::app::SerialSession::disconnectNetwork);
    connect(networkPanel_, &NetworkPanel::reconnectRequested, this, [this] {
        sendPanel_->setTarget(1);
        session_.reconnectNetwork();
    });
    connect(remoteAgentPanel_, &RemoteAgentPanel::connectRequested, this, [this] {
        session_.connectRemoteAgent(remoteAgentPanel_->settings());
    });
    connect(remoteAgentPanel_, &RemoteAgentPanel::disconnectRequested,
            &session_, &lab::app::SerialSession::disconnectRemoteAgent);
    connect(remoteAgentPanel_, &RemoteAgentPanel::reconnectRequested,
            &session_, &lab::app::SerialSession::reconnectRemoteAgent);
    connect(remoteAgentPanel_, &RemoteAgentPanel::refreshTopicsRequested,
            &session_, &lab::app::SerialSession::requestRemoteTopics);
    connect(remoteAgentPanel_, &RemoteAgentPanel::subscribeRequested,
            &session_, &lab::app::SerialSession::subscribeRemoteTopic);
    connect(remoteAgentPanel_, &RemoteAgentPanel::unsubscribeRequested,
            &session_, &lab::app::SerialSession::unsubscribeRemoteTopic);
    connect(sendPanel_, &SendPanel::sendRequested,
            &session_, &lab::app::SerialSession::sendBytes);
    connect(sendPanel_, &SendPanel::targetChanged,
            &session_, &lab::app::SerialSession::setSendTarget);
    connect(plot_, &PlotWidget::fieldsChanged,
            &session_, &lab::app::SerialSession::setCsvFields);
    connect(derivedFields_, &DerivedFieldsWidget::applyRequested,
            &session_, &lab::app::SerialSession::setDerivedFields);
    connect(alerts_, &AlertsWidget::addMarkerRequested,
            &session_, &lab::app::SerialSession::addManualMarker);
    connect(alerts_, &AlertsWidget::applyRequested,
            &session_, &lab::app::SerialSession::setAlertRules);
    connect(protocol_, &ProtocolWidget::csvConfigurationRequested,
            this, [this](const QString& sourceId, const QStringList& fields) {
                if (sourceId.isEmpty()) {
                    session_.setCsvFields(fields);
                } else {
                    session_.setSourceCsvFields(sourceId, fields);
                }
            });
    connect(protocol_, &ProtocolWidget::protocolConfigurationRequested,
            this, [this](const QString& sourceId, const QString& path) {
                if (sourceId.isEmpty()) {
                    session_.loadProtocolFile(path);
                } else {
                    session_.loadSourceProtocolFile(sourceId, path);
                }
            });
    connect(protocol_, &ProtocolWidget::disableProtocolConfigurationRequested,
            this, [this](const QString& sourceId) {
                if (sourceId.isEmpty()) {
                    session_.clearProtocol();
                } else {
                    session_.clearSourceProtocol(sourceId);
                }
            });
    connect(protocol_, &ProtocolWidget::resetSourceConfigurationRequested,
            &session_, &lab::app::SerialSession::resetSourceParserConfiguration);
    connect(replay_, &ReplayWidget::openSessionRequested,
            &session_, &lab::app::SerialSession::openReplaySession);
    connect(replay_, &ReplayWidget::inspectRosbagRequested,
            &session_, &lab::app::SerialSession::inspectRosbag2);
    connect(replay_, &ReplayWidget::importRosbagRequested,
            &session_, &lab::app::SerialSession::importRosbag2);
    connect(replay_, &ReplayWidget::cancelRosbagImportRequested,
            &session_, &lab::app::SerialSession::cancelRosbag2Import);
    connect(replay_, &ReplayWidget::closeReplayRequested,
            &session_, &lab::app::SerialSession::closeReplay);
    connect(replay_, &ReplayWidget::pauseRequested,
            &session_, &lab::app::SerialSession::pauseReplay);
    connect(replay_, &ReplayWidget::resumeRequested,
            &session_, &lab::app::SerialSession::resumeReplay);
    connect(replay_, &ReplayWidget::speedChanged,
            &session_, &lab::app::SerialSession::setReplaySpeed);
    connect(replay_, &ReplayWidget::seekRequested,
            &session_, &lab::app::SerialSession::seekReplay);
    connect(&session_, &lab::app::SerialSession::chunkReady,
            terminal_, &TerminalWidget::appendChunk);
    connect(&session_, &lab::app::SerialSession::sourceStateChanged,
            serialPanel_, &SerialPanel::setSourceState);
    connect(&session_, &lab::app::SerialSession::networkStateChanged,
            networkPanel_, &NetworkPanel::setSourceState);
    connect(&session_, &lab::app::SerialSession::remoteAgentStateChanged,
            remoteAgentPanel_, &RemoteAgentPanel::setSourceState);
    connect(&session_, &lab::app::SerialSession::remoteAgentHello,
            remoteAgentPanel_, &RemoteAgentPanel::setAgentHello);
    connect(&session_, &lab::app::SerialSession::remoteAgentClockSync,
            remoteAgentPanel_, &RemoteAgentPanel::setClockSync);
    connect(&session_, &lab::app::SerialSession::remoteTopicsChanged,
            remoteAgentPanel_, &RemoteAgentPanel::setTopics);
    connect(&session_, &lab::app::SerialSession::remoteTopicFieldsChanged,
            remoteAgentPanel_, &RemoteAgentPanel::setTopicFields);
    connect(&session_, &lab::app::SerialSession::liveFieldsDiscovered,
            plot_, &PlotWidget::useExternalFields);
    connect(&session_, &lab::app::SerialSession::replayFieldsDiscovered,
            plot_, &PlotWidget::useExternalFields);
    connect(&session_, &lab::app::SerialSession::protocolLoaded,
            protocol_, &ProtocolWidget::setProtocolLoaded);
    connect(&session_, &lab::app::SerialSession::protocolLoaded,
            this, [this](const QString&, const QStringList& fields) {
                plot_->useProtocolFields(fields);
                statusBar()->showMessage(tr("协议已加载，数值字段已接入实时曲线"), 4000);
            });
    connect(&session_, &lab::app::SerialSession::protocolCleared,
            protocol_, &ProtocolWidget::setProtocolCleared);
    connect(&session_, &lab::app::SerialSession::protocolLoadFailed,
            protocol_, &ProtocolWidget::showLoadErrors);
    connect(&session_, &lab::app::SerialSession::protocolEventsReady,
            protocol_, &ProtocolWidget::appendEvents);
    connect(&session_, &lab::app::SerialSession::protocolStatisticsChanged,
            protocol_, &ProtocolWidget::setStatistics);
    connect(&session_, &lab::app::SerialSession::parserSourcesChanged,
            protocol_, &ProtocolWidget::setParserSources);
    connect(&session_, &lab::app::SerialSession::sourceParserConfigured,
            protocol_, &ProtocolWidget::showParserConfigured);
    connect(&session_, &lab::app::SerialSession::sourceParserConfigurationFailed,
            protocol_, &ProtocolWidget::showParserErrors);
    connect(&session_, &lab::app::SerialSession::replayOpened,
            replay_, &ReplayWidget::setOpened);
    connect(&session_, &lab::app::SerialSession::replayOpenFailed,
            replay_, &ReplayWidget::showOpenError);
    connect(&session_, &lab::app::SerialSession::replayStatusChanged,
            replay_, &ReplayWidget::setStatus);
    connect(&session_, &lab::app::SerialSession::rosbagInspectionStarted,
            replay_, &ReplayWidget::setInspectionStarted);
    connect(&session_, &lab::app::SerialSession::rosbagInspectionFinished,
            replay_, &ReplayWidget::showInspectionResult);
    connect(&session_, &lab::app::SerialSession::rosbagImportStarted,
            replay_, &ReplayWidget::setImportStarted);
    connect(&session_, &lab::app::SerialSession::rosbagImportProgress,
            replay_, &ReplayWidget::setImportProgress);
    connect(&session_, &lab::app::SerialSession::rosbagImportFinished,
            replay_, &ReplayWidget::setImportFinished);
    connect(&session_, &lab::app::SerialSession::csvFieldsRestored,
            plot_, &PlotWidget::useProtocolFields);
    connect(&session_, &lab::app::SerialSession::derivedFieldsConfigured,
            derivedFields_, &DerivedFieldsWidget::showConfigurationResult);
    connect(&session_, &lab::app::SerialSession::derivedFieldsRestored,
            derivedFields_, &DerivedFieldsWidget::setDefinitions);
    connect(&session_, &lab::app::SerialSession::alertRulesConfigured,
            alerts_, &AlertsWidget::showConfigurationResult);
    connect(&session_, &lab::app::SerialSession::alertRulesRestored,
            alerts_, &AlertsWidget::setDefinitions);
    connect(&session_, &lab::app::SerialSession::timelineEventsChanged,
            alerts_, &AlertsWidget::setTimelineEvents);
    connect(&session_, &lab::app::SerialSession::timelineEventsChanged,
            plot_, &PlotWidget::setTimelineEvents);
    connect(&session_, &lab::app::SerialSession::recordingChanged,
            this, [this](bool active, const QString& message) {
                recordAction_->setText(active ? tr("停止 Session 记录")
                                              : tr("开始 Session 记录"));
                statusBar()->showMessage(message, active ? 0 : 5000);
            });
    connect(&session_, &lab::app::SerialSession::sourceError, this, [this](const QString& message) {
        statusBar()->showMessage(tr("错误：%1").arg(message), 8000);
    });
    connect(&session_, &lab::app::SerialSession::statisticsChanged,
            this,
            [this](quint64 rx, quint64 tx, qsizetype backlog) {
                traffic_->setText(tr("RX %1 B  |  TX %2 B").arg(rx).arg(tx));
                parserStatus_->setText(tr("解析队列 %1").arg(backlog));
                parserStatus_->setStyleSheet(
                    backlog > 1000 ? QStringLiteral("color: #ff6b6b;") : QString());
            });
    connect(recordAction_, &QAction::toggled, this, &MainWindow::toggleRecording);
    connect(markerAction, &QAction::triggered, this, [this] {
        bool accepted = false;
        const auto message = QInputDialog::getText(
            this,
            tr("添加 Marker"),
            tr("说明"),
            QLineEdit::Normal,
            {},
            &accepted);
        if (accepted && !message.trimmed().isEmpty()) {
            session_.addManualMarker(message);
        }
    });
    connect(architectureAction, &QAction::triggered, this, [this] {
        QMessageBox::information(
            this,
            tr("线程与数据状态"),
            tr("串口/网络 I/O、CSV/二进制协议处理、原始记录分别运行在独立线程。\n"
               "串口、网络和 ROS Agent 可同时连接；各来源的半行/半帧状态互相隔离。\n"
               "GUI 每 33 ms 批量刷新；暂停显示不会暂停采集或记录。\n"
               "所有数据均携带 sourceId、源时间、接收时间和序号。"));
    });
    session_.setCsvFields(plot_->fieldNames());
    session_.setSendTarget(sendPanel_->target());
}

void MainWindow::closeEvent(QCloseEvent* event) {
    session_.stopSession();
    session_.disconnectSerial();
    session_.disconnectNetwork();
    session_.disconnectRemoteAgent();
    session_.closeReplay();
    event->accept();
}

void MainWindow::toggleRecording(bool enabled) {
    if (!enabled) {
        session_.stopSession();
        return;
    }

    const auto documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const auto parent = QFileDialog::getExistingDirectory(
        this, tr("选择 Session 保存位置"), documents);
    const auto sessionName = QStringLiteral("session_") +
                             QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    const auto path = parent.isEmpty() ? QString() : QDir(parent).filePath(sessionName);
    if (path.isEmpty() || !session_.startSession(path)) {
        recordAction_->blockSignals(true);
        recordAction_->setChecked(false);
        recordAction_->blockSignals(false);
        if (!path.isEmpty()) {
            QMessageBox::warning(this, tr("记录失败"), tr("无法创建 Session，详情请查看状态栏。"));
        }
        return;
    }
}

}  // namespace lab::ui

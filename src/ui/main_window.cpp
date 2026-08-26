#include "ui/main_window.hpp"

#include "ui/plot_widget.hpp"
#include "ui/send_panel.hpp"
#include "ui/serial_panel.hpp"
#include "ui/terminal_widget.hpp"

#include <QAction>
#include <QCloseEvent>
#include <QDateTime>
#include <QFileDialog>
#include <QLabel>
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
    recordAction_ = toolbar->addAction(tr("开始原始记录"));
    recordAction_->setCheckable(true);
    toolbar->addSeparator();
    auto* architectureAction = toolbar->addAction(tr("架构状态"));

    serialPanel_ = new SerialPanel(this);
    terminal_ = new TerminalWidget(this);
    plot_ = new PlotWidget(&session_.timeSeries(), this);
    sendPanel_ = new SendPanel(this);

    auto* tabs = new QTabWidget(this);
    tabs->addTab(terminal_, tr("终端"));
    tabs->addTab(plot_, tr("实时曲线"));

    auto* right = new QWidget(this);
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->addWidget(tabs, 1);
    rightLayout->addWidget(sendPanel_);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(serialPanel_);
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
        session_.connectSerial(configuration);
    });
    connect(serialPanel_, &SerialPanel::disconnectRequested,
            &session_, &lab::app::SerialSession::disconnectSerial);
    connect(serialPanel_, &SerialPanel::reconnectRequested,
            &session_, &lab::app::SerialSession::reconnectSerial);
    connect(sendPanel_, &SendPanel::sendRequested,
            &session_, &lab::app::SerialSession::sendBytes);
    connect(plot_, &PlotWidget::fieldsChanged,
            &session_, &lab::app::SerialSession::setCsvFields);
    connect(&session_, &lab::app::SerialSession::chunkReady,
            terminal_, &TerminalWidget::appendChunk);
    connect(&session_, &lab::app::SerialSession::sourceStateChanged,
            serialPanel_, &SerialPanel::setSourceState);
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
    connect(architectureAction, &QAction::triggered, this, [this] {
        QMessageBox::information(
            this,
            tr("线程与数据状态"),
            tr("串口 I/O、CSV 处理、原始记录分别运行在独立线程。\n"
               "GUI 每 33 ms 批量刷新；暂停显示不会暂停采集或记录。\n"
               "所有数据均携带 sourceId、源时间、接收时间和序号。"));
    });
}

void MainWindow::closeEvent(QCloseEvent* event) {
    session_.disconnectSerial();
    session_.stopRecording();
    event->accept();
}

void MainWindow::toggleRecording(bool enabled) {
    if (!enabled) {
        session_.stopRecording();
        recordAction_->setText(tr("开始原始记录"));
        statusBar()->showMessage(tr("原始记录已停止"), 3000);
        return;
    }

    const auto directory = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const auto defaultName = directory + QStringLiteral("/lab_debug_") +
                             QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")) +
                             QStringLiteral(".ldraw");
    const auto path = QFileDialog::getSaveFileName(
        this, tr("保存原始记录"), defaultName, tr("Lab Debug Raw (*.ldraw);;所有文件 (*)"));
    if (path.isEmpty() || !session_.startRecording(path)) {
        recordAction_->blockSignals(true);
        recordAction_->setChecked(false);
        recordAction_->blockSignals(false);
        if (!path.isEmpty()) {
            QMessageBox::warning(this, tr("记录失败"), tr("无法创建记录文件。"));
        }
        return;
    }
    recordAction_->setText(tr("停止原始记录"));
    statusBar()->showMessage(tr("正在记录：%1").arg(path));
}

}  // namespace lab::ui


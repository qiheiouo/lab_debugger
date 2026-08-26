#pragma once

#include "app/serial_session.hpp"

#include <QMainWindow>

class QAction;
class QLabel;

namespace lab::ui {

class PlotWidget;
class NetworkPanel;
class RemoteAgentPanel;
class ProtocolWidget;
class ReplayWidget;
class SendPanel;
class SerialPanel;
class TerminalWidget;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void toggleRecording(bool enabled);

private:
    lab::app::SerialSession session_;
    SerialPanel* serialPanel_{};
    NetworkPanel* networkPanel_{};
    RemoteAgentPanel* remoteAgentPanel_{};
    TerminalWidget* terminal_{};
    PlotWidget* plot_{};
    ProtocolWidget* protocol_{};
    ReplayWidget* replay_{};
    SendPanel* sendPanel_{};
    QLabel* traffic_{};
    QLabel* parserStatus_{};
    QAction* recordAction_{};
};

}  // namespace lab::ui

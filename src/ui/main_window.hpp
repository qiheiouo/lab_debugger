#pragma once

#include "app/serial_session.hpp"

#include <QMainWindow>

class QAction;
class QLabel;

namespace lab::ui {

class PlotWidget;
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
    TerminalWidget* terminal_{};
    PlotWidget* plot_{};
    SendPanel* sendPanel_{};
    QLabel* traffic_{};
    QLabel* parserStatus_{};
    QAction* recordAction_{};
};

}  // namespace lab::ui


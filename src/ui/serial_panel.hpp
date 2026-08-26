#pragma once

#include "lab/adapters/serial/serial_source.hpp"

#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;

namespace lab::ui {

class SerialPanel final : public QWidget {
    Q_OBJECT

public:
    explicit SerialPanel(QWidget* parent = nullptr);

    [[nodiscard]] lab::adapters::serial::SerialSettings settings() const;

public slots:
    void refreshPorts();
    void setSourceState(int state);

signals:
    void connectRequested();
    void disconnectRequested();
    void reconnectRequested();

private:
    QComboBox* port_{};
    QComboBox* baud_{};
    QComboBox* dataBits_{};
    QComboBox* stopBits_{};
    QComboBox* parity_{};
    QComboBox* flowControl_{};
    QLabel* state_{};
    QPushButton* connectButton_{};
    QPushButton* disconnectButton_{};
    QPushButton* reconnectButton_{};
};

}  // namespace lab::ui


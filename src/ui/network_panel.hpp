#pragma once

#include "lab/adapters/network/network_source.hpp"

#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace lab::ui {

class NetworkPanel final : public QWidget {
    Q_OBJECT

public:
    explicit NetworkPanel(QWidget* parent = nullptr);

    [[nodiscard]] lab::adapters::network::NetworkSettings settings() const;

signals:
    void connectRequested();
    void disconnectRequested();
    void reconnectRequested();

public slots:
    void setSourceState(int state);

private slots:
    void updateModeControls();

private:
    QComboBox* mode_{};
    QLineEdit* remoteHost_{};
    QSpinBox* remotePort_{};
    QLineEdit* bindAddress_{};
    QSpinBox* localPort_{};
    QLabel* state_{};
    QPushButton* connectButton_{};
    QPushButton* disconnectButton_{};
    QPushButton* reconnectButton_{};
};

}  // namespace lab::ui

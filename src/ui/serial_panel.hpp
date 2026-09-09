#pragma once

#include "lab/adapters/serial/serial_source.hpp"

#include <QWidget>
#include <QVariantList>

class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;

namespace lab::ui {

class SerialPanel final : public QWidget {
    Q_OBJECT

public:
    explicit SerialPanel(QWidget* parent = nullptr);

    [[nodiscard]] lab::adapters::serial::SerialSettings settings() const;
    [[nodiscard]] QString selectedSourceId() const;

public slots:
    void refreshPorts();
    void setSourceState(int state);
    void setSources(const QVariantList& sources);

signals:
    void connectRequested();
    void disconnectSourceRequested(QString sourceId);
    void reconnectSourceRequested(QString sourceId);
    void removeSourceRequested(QString sourceId);

private slots:
    void loadSelectedSource();

private:
    QComboBox* port_{};
    QComboBox* baud_{};
    QComboBox* dataBits_{};
    QComboBox* stopBits_{};
    QComboBox* parity_{};
    QComboBox* flowControl_{};
    QListWidget* sources_{};
    QLabel* state_{};
    QPushButton* connectButton_{};
    QPushButton* disconnectButton_{};
    QPushButton* reconnectButton_{};
    QPushButton* removeButton_{};
};

}  // namespace lab::ui

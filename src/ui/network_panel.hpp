#pragma once

#include "lab/adapters/network/network_source.hpp"

#include <QWidget>
#include <QVariantList>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;

namespace lab::ui {

class NetworkPanel final : public QWidget {
    Q_OBJECT

public:
    explicit NetworkPanel(QWidget* parent = nullptr);

    [[nodiscard]] lab::adapters::network::NetworkSettings settings() const;
    [[nodiscard]] QString selectedSourceId() const;

signals:
    void connectRequested();
    void disconnectSourceRequested(QString sourceId);
    void reconnectSourceRequested(QString sourceId);
    void removeSourceRequested(QString sourceId);

public slots:
    void setSourceState(int state);
    void setSources(const QVariantList& sources);

private slots:
    void updateModeControls();
    void loadSelectedSource();

private:
    QComboBox* mode_{};
    QLineEdit* remoteHost_{};
    QSpinBox* remotePort_{};
    QLineEdit* bindAddress_{};
    QSpinBox* localPort_{};
    QListWidget* sources_{};
    QLabel* state_{};
    QPushButton* connectButton_{};
    QPushButton* disconnectButton_{};
    QPushButton* reconnectButton_{};
    QPushButton* removeButton_{};
};

}  // namespace lab::ui

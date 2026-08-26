#pragma once

#include <QByteArray>
#include <QWidget>

#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QSpinBox;
class QTimer;

namespace lab::ui {

class SendPanel final : public QWidget {
    Q_OBJECT

public:
    explicit SendPanel(QWidget* parent = nullptr);

signals:
    void sendRequested(QByteArray bytes);

private slots:
    void sendNow();
    void addFavorite();
    void updateTimer();

private:
    [[nodiscard]] std::optional<QByteArray> payload() const;
    void rememberHistory(const QString& text);
    void saveFavorites() const;

    QComboBox* mode_{};
    QComboBox* input_{};
    QComboBox* lineEnding_{};
    QComboBox* favorites_{};
    QCheckBox* periodic_{};
    QSpinBox* periodMs_{};
    QLabel* validation_{};
    QTimer* timer_{};
};

}  // namespace lab::ui


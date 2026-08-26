#pragma once

#include <QWidget>

class QLabel;
class QPushButton;
class QSlider;

namespace lab::ui {

class ReplayWidget final : public QWidget {
    Q_OBJECT

public:
    explicit ReplayWidget(QWidget* parent = nullptr);

signals:
    void openSessionRequested(QString directory);
    void closeReplayRequested();
    void pauseRequested();
    void resumeRequested();
    void speedChanged(double speed);
    void seekRequested(double fraction);

public slots:
    void setOpened(const QString& directory, bool recoveredTruncatedTail);
    void showOpenError(const QString& message);
    void setStatus(bool open,
                   bool paused,
                   bool atEnd,
                   double speed,
                   quint64 position,
                   quint64 recordCount,
                   qint64 firstTimestamp,
                   qint64 lastTimestamp,
                   qint64 currentTimestamp);

private:
    QLabel* pathLabel_{};
    QLabel* stateLabel_{};
    QLabel* timeLabel_{};
    QLabel* countLabel_{};
    QPushButton* playButton_{};
    QPushButton* closeButton_{};
    QSlider* timeline_{};
    bool paused_{true};
};

}  // namespace lab::ui

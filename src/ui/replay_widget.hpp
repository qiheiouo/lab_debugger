#pragma once

#include <QWidget>
#include <QVariantList>

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
    void inspectRosbagRequested(QString source, QString destination);
    void importRosbagRequested(QString source,
                               QString destination,
                               QVariantList selectedTopics);
    void cancelRosbagImportRequested();
    void closeReplayRequested();
    void pauseRequested();
    void resumeRequested();
    void speedChanged(double speed);
    void seekRequested(double fraction);

public slots:
    void setOpened(const QString& directory,
                   bool recoveredTruncatedTail,
                   bool rawOnly,
                   bool structuredRosbag);
    void showOpenError(const QString& message);
    void setInspectionStarted(const QString& source, const QString& destination);
    void showInspectionResult(bool success,
                              const QString& source,
                              const QString& destination,
                              const QString& message,
                              const QVariantList& topics,
                              quint64 databaseCount,
                              quint64 messageCount);
    void setImportStarted(const QString& source, const QString& destination);
    void setImportProgress(quint64 importedMessages, quint64 totalMessages);
    void setImportFinished(bool success,
                           const QString& directory,
                           const QString& message,
                           quint64 messageCount,
                           quint64 topicCount);
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
    void chooseRosbagSource(bool directory);

    QLabel* pathLabel_{};
    QLabel* stateLabel_{};
    QLabel* timeLabel_{};
    QLabel* countLabel_{};
    QPushButton* playButton_{};
    QPushButton* closeButton_{};
    QPushButton* importButton_{};
    QPushButton* importDirectoryButton_{};
    QPushButton* cancelImportButton_{};
    QLabel* importLabel_{};
    QSlider* timeline_{};
    bool paused_{true};
};

}  // namespace lab::ui

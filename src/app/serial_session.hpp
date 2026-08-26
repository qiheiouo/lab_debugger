#pragma once

#include "lab/adapters/serial/serial_source.hpp"
#include "lab/adapters/network/network_source.hpp"
#include "lab/core/data_chunk.hpp"
#include "lab/core/processing_pipeline.hpp"
#include "lab/core/replay_source.hpp"
#include "lab/core/session_recorder.hpp"
#include "lab/core/time_series_store.hpp"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

#include <deque>
#include <mutex>
#include <vector>

namespace lab::app {

class SerialSession final : public QObject {
    Q_OBJECT

public:
    explicit SerialSession(QObject* parent = nullptr);
    ~SerialSession() override;

    [[nodiscard]] static std::vector<lab::adapters::serial::PortInfo> availablePorts();
    [[nodiscard]] const lab::core::TimeSeriesStore& timeSeries() const noexcept;

public slots:
    void connectSerial(lab::adapters::serial::SerialSettings settings);
    void disconnectSerial();
    void reconnectSerial();
    void connectNetwork(lab::adapters::network::NetworkSettings settings);
    void disconnectNetwork();
    void reconnectNetwork();
    void sendBytes(const QByteArray& bytes);
    void setCsvFields(const QStringList& fields);
    void loadProtocolFile(const QString& path);
    void clearProtocol();
    bool startSession(const QString& directory);
    void stopSession();
    bool openReplaySession(const QString& directory);
    void closeReplay();
    void pauseReplay();
    void resumeReplay();
    void setReplaySpeed(double speed);
    void seekReplay(double fraction);

signals:
    void chunkReady(QByteArray bytes, bool transmitted, qint64 timestampNs);
    void sourceStateChanged(int state);
    void networkStateChanged(int state);
    void sourceError(QString message);
    void statisticsChanged(quint64 rxBytes, quint64 txBytes, qsizetype parserBacklog);
    void recordingChanged(bool active, QString message);
    void protocolLoaded(QString name, QStringList numericFields);
    void protocolCleared();
    void protocolLoadFailed(QStringList issues);
    void protocolEventsReady(QVariantList events);
    void protocolStatisticsChanged(quint64 decoded,
                                   quint64 discardedBytes,
                                   quint64 checksumErrors,
                                   quint64 lengthErrors,
                                   quint64 decodeErrors);
    void replayOpened(QString directory, bool recoveredTruncatedTail);
    void replayOpenFailed(QString message);
    void replayStatusChanged(bool open,
                             bool paused,
                             bool atEnd,
                             double speed,
                             quint64 position,
                             quint64 recordCount,
                             qint64 firstTimestamp,
                             qint64 lastTimestamp,
                             qint64 currentTimestamp);
    void csvFieldsRestored(QStringList fields);

private slots:
    void drainUiQueue();

private:
    enum class LiveSourceKind { Serial, Network };

    lab::adapters::serial::SerialSource source_;
    lab::adapters::network::NetworkSource network_;
    lab::core::ReplaySource replay_;
    lab::adapters::serial::SerialSettings lastSettings_;
    lab::adapters::network::NetworkSettings lastNetworkSettings_;
    bool networkConfigured_{};
    LiveSourceKind activeLiveSource_{LiveSourceKind::Serial};
    lab::core::TimeSeriesStore timeSeries_{120'000};
    std::mutex uiQueueMutex_;
    std::deque<lab::core::DataChunk> uiQueue_;
    std::mutex protocolQueueMutex_;
    std::deque<lab::core::FrameEvent> protocolQueue_;
    std::mutex routingMutex_;
    lab::core::SessionRecorder recorder_;
    lab::core::ProcessingPipeline processing_{timeSeries_};
    std::string activeProtocolName_;
    std::string activeProtocolJson_;
    std::vector<std::string> activeCsvFields_;
    QTimer refreshTimer_;
};

}  // namespace lab::app

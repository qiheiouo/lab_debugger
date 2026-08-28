#pragma once

#include "lab/adapters/serial/serial_source.hpp"
#include "lab/adapters/network/network_source.hpp"
#include "lab/adapters/remote_agent/remote_agent_source.hpp"
#include "lab/adapters/rosbag2/rosbag2_importer.hpp"
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
#include <atomic>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <utility>
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
    void connectRemoteAgent(lab::adapters::remote_agent::RemoteAgentSettings settings);
    void disconnectRemoteAgent();
    void reconnectRemoteAgent();
    void requestRemoteTopics();
    void subscribeRemoteTopic(lab::core::agent::SubscriptionRequest request);
    void unsubscribeRemoteTopic(lab::core::agent::SubscriptionRequest request);
    void sendBytes(const QByteArray& bytes);
    void setCsvFields(const QStringList& fields);
    void loadProtocolFile(const QString& path);
    void clearProtocol();
    bool startSession(const QString& directory);
    void stopSession();
    bool openReplaySession(const QString& directory);
    void inspectRosbag2(const QString& source, const QString& destination);
    void importRosbag2(const QString& source,
                       const QString& destination,
                       const QVariantList& selectedTopics);
    void cancelRosbag2Import();
    void closeReplay();
    void pauseReplay();
    void resumeReplay();
    void setReplaySpeed(double speed);
    void seekReplay(double fraction);

signals:
    void chunkReady(QByteArray bytes,
                    bool transmitted,
                    qint64 timestampNs,
                    QString sourceId);
    void sourceStateChanged(int state);
    void networkStateChanged(int state);
    void remoteAgentStateChanged(int state);
    void remoteAgentHello(QString agentId,
                          QString softwareVersion,
                          QString hostName,
                          quint32 capabilities);
    void remoteAgentClockSync(qint64 offsetNs,
                              qint64 roundTripNs,
                              qint64 uncertaintyNs,
                              quint32 sampleCount);
    void remoteTopicsChanged(QVariantList topics, quint64 graphRevision);
    void remoteTopicFieldsChanged(QVariantList topics, quint64 graphRevision);
    void remoteFieldsDiscovered(QStringList fields);
    void replayFieldsDiscovered(QStringList fields);
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
    void replayOpened(QString directory,
                      bool recoveredTruncatedTail,
                      bool rawOnly,
                      bool structuredRosbag);
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
    void rosbagInspectionStarted(QString source, QString destination);
    void rosbagInspectionFinished(bool success,
                                  QString source,
                                  QString destination,
                                  QString message,
                                  QVariantList topics,
                                  quint64 databaseCount,
                                  quint64 messageCount);
    void rosbagImportStarted(QString source, QString destination);
    void rosbagImportProgress(quint64 importedMessages, quint64 totalMessages);
    void rosbagImportFinished(bool success,
                              QString directory,
                              QString message,
                              quint64 messageCount,
                              quint64 topicCount);

private slots:
    void drainUiQueue();

private:
    enum class LiveSourceKind { Serial, Network, RemoteAgent };

    [[nodiscard]] std::string activeSourceId() const;

    lab::adapters::serial::SerialSource source_;
    lab::adapters::network::NetworkSource network_;
    lab::adapters::remote_agent::RemoteAgentSource remoteAgent_;
    lab::core::ReplaySource replay_;
    std::atomic_bool replayRawOnly_{};
    std::atomic_bool replayStructuredRosbag_{};
    std::atomic_bool rosbagImporting_{};
    std::jthread rosbagImportWorker_;
    lab::adapters::serial::SerialSettings lastSettings_;
    lab::adapters::network::NetworkSettings lastNetworkSettings_;
    lab::adapters::remote_agent::RemoteAgentSettings lastRemoteAgentSettings_;
    bool networkConfigured_{};
    bool remoteAgentConfigured_{};
    LiveSourceKind activeLiveSource_{LiveSourceKind::Serial};
    lab::core::TimeSeriesStore timeSeries_{120'000};
    std::mutex uiQueueMutex_;
    std::deque<lab::core::DataChunk> uiQueue_;
    std::mutex protocolQueueMutex_;
    std::deque<lab::core::FrameEvent> protocolQueue_;
    std::mutex routingMutex_;
    std::mutex remoteFieldsMutex_;
    std::set<std::string> remoteFieldNames_;
    std::unordered_map<std::string, std::pair<std::string, std::string>>
        rosbagReplayTopics_;
    std::set<std::string> replayFieldNames_;
    std::set<std::string> replayMappingWarnings_;
    lab::core::SessionRecorder recorder_;
    lab::core::ProcessingPipeline processing_{timeSeries_};
    std::string activeProtocolName_;
    std::string activeProtocolJson_;
    std::vector<std::string> activeCsvFields_;
    QTimer refreshTimer_;
};

}  // namespace lab::app

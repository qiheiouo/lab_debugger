#pragma once

#include "lab/adapters/serial/serial_source.hpp"
#include "lab/adapters/network/network_source.hpp"
#include "lab/adapters/remote_agent/remote_agent_source.hpp"
#include "lab/adapters/rosbag2/rosbag2_importer.hpp"
#include "lab/core/data_chunk.hpp"
#include "lab/core/derived_field_engine.hpp"
#include "lab/core/health_alert_engine.hpp"
#include "lab/core/processing_pipeline.hpp"
#include "lab/core/replay_source.hpp"
#include "lab/core/session_recorder.hpp"
#include "lab/core/source_manager.hpp"
#include "lab/core/time_series_store.hpp"
#include "lab/core/threshold_alert_engine.hpp"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

#include <deque>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
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
    void disconnectLocalSource(const QString& sourceId);
    void reconnectLocalSource(const QString& sourceId);
    bool removeLocalSource(const QString& sourceId);
    void disconnectAllLocalSources();
    void connectRemoteAgent(lab::adapters::remote_agent::RemoteAgentSettings settings);
    void disconnectRemoteAgent();
    void reconnectRemoteAgent();
    void setSendTarget(int target);
    void setSendTargetSource(const QString& sourceId);
    void requestRemoteTopics();
    void subscribeRemoteTopic(lab::core::agent::SubscriptionRequest request);
    void unsubscribeRemoteTopic(lab::core::agent::SubscriptionRequest request);
    void sendBytes(const QByteArray& bytes);
    void setCsvFields(const QStringList& fields);
    bool setSourceCsvFields(const QString& sourceId, const QStringList& fields);
    bool loadSourceProtocolFile(const QString& sourceId, const QString& path);
    bool clearSourceProtocol(const QString& sourceId);
    bool resetSourceParserConfiguration(const QString& sourceId);
    bool setDerivedFields(const QVariantList& definitions);
    bool setAlertRules(const QVariantList& definitions);
    bool setHealthAlertRules(const QVariantList& definitions);
    bool addManualMarker(const QString& message);
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
    void localSourcesChanged(QVariantList sources);
    void localSourceStateChanged(QString sourceId, QString type, int state);
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
    void liveFieldsDiscovered(QStringList fields);
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
    void parserSourcesChanged(QVariantList sources);
    void sourceParserConfigured(QString sourceId,
                                QString mode,
                                QString name,
                                QStringList fields);
    void sourceParserConfigurationFailed(QString sourceId, QStringList issues);
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
    void derivedFieldsConfigured(bool success, QStringList messages);
    void derivedFieldsRestored(QVariantList definitions);
    void alertRulesConfigured(bool success, QStringList messages);
    void alertRulesRestored(QVariantList definitions);
    void healthAlertRulesConfigured(bool success, QStringList messages);
    void healthAlertRulesRestored(QVariantList definitions);
    void timelineEventsChanged(QVariantList events);
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
    struct ActiveParserConfiguration {
        std::vector<std::string> csvFields;
        std::optional<lab::core::ProtocolDefinition> protocolDefinition;
        std::string protocolName;
        std::string protocolJson;
    };

    void discoverLiveField(const std::string& field);
    [[nodiscard]] std::vector<lab::core::DataSample> appendDerivedSamples(
        std::span<const lab::core::DataSample> inputs,
        bool record);
    void processAlerts(std::span<const lab::core::DataSample> samples, bool record);
    void processHealthAlerts();
    void armHealthAlertTargets();
    [[nodiscard]] bool shouldRecordHealthAlert(
        const std::string& sourceId) const noexcept;
    void publishTimelineEvent(lab::core::SessionEvent event, bool record);
    void clearTimelineEvents();
    void leaveReplayForLiveSource();
    void publishLocalSources();
    void publishParserSources();
    void clearSourceParserConfigurations();
    void resetParserDependentState();
    [[nodiscard]] lab::core::SessionParserConfiguration
    resolvedParserConfiguration(const std::string& sourceId) const;
    [[nodiscard]] bool isDeclaredRecordingSource(const std::string& key) const noexcept;
    [[nodiscard]] bool isLocalSource(const std::string& key) const noexcept;
    [[nodiscard]] QString localSourceType(const std::string& key) const;
    void selectFallbackSendTarget();

    std::map<std::string, std::unique_ptr<lab::adapters::serial::SerialSource>>
        serialSources_;
    std::map<std::string, std::unique_ptr<lab::adapters::network::NetworkSource>>
        networkSources_;
    lab::adapters::remote_agent::RemoteAgentSource remoteAgent_;
    lab::core::SourceManager sourceManager_;
    lab::core::ReplaySource replay_;
    std::atomic_bool replayRawOnly_{};
    std::atomic_bool replayStructuredRosbag_{};
    std::atomic_bool rosbagImporting_{};
    std::jthread rosbagImportWorker_;
    lab::adapters::remote_agent::RemoteAgentSettings lastRemoteAgentSettings_;
    bool remoteAgentConfigured_{};
    std::string selectedSerialSource_;
    std::string selectedNetworkSource_;
    std::string sendTargetSource_;
    std::map<std::string, lab::core::SourceState> localSourceStates_;
    mutable std::mutex recordingSourcesMutex_;
    std::set<std::string> recordingSourceKeys_;
    lab::core::TimeSeriesStore timeSeries_{120'000};
    lab::core::DerivedFieldEngine derivedFields_;
    lab::core::ThresholdAlertEngine alertRules_;
    lab::core::HealthAlertEngine healthAlertRules_;
    std::set<std::pair<std::string, std::string>> remoteSubscriptions_;
    std::atomic<lab::core::Timestamp> latestLiveTimestamp_{};
    std::atomic_uint64_t nextTimelineSequence_{};
    std::atomic_bool replaying_{};
    std::mutex timelineEventsMutex_;
    std::deque<lab::core::SessionEvent> timelineEvents_;
    std::mutex uiQueueMutex_;
    std::deque<lab::core::DataChunk> uiQueue_;
    std::mutex protocolQueueMutex_;
    std::deque<lab::core::FrameEvent> protocolQueue_;
    std::mutex routingMutex_;
    std::mutex liveFieldsMutex_;
    std::set<std::string> liveFieldNames_;
    std::unordered_map<std::string, std::pair<std::string, std::string>>
        rosbagReplayTopics_;
    std::set<std::string> replayFieldNames_;
    std::set<std::string> replayMappingWarnings_;
    lab::core::SessionRecorder recorder_;
    lab::core::ProcessingPipeline processing_{timeSeries_};
    std::string activeProtocolName_;
    std::string activeProtocolJson_;
    std::vector<std::string> activeCsvFields_{"field0", "field1", "field2"};
    std::unordered_map<std::string, ActiveParserConfiguration>
        sourceParserConfigurations_;
    QTimer refreshTimer_;
};

}  // namespace lab::app

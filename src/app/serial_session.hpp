#pragma once

#include "lab/adapters/serial/serial_source.hpp"
#include "lab/core/data_chunk.hpp"
#include "lab/core/processing_pipeline.hpp"
#include "lab/core/raw_log_recorder.hpp"
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
    void sendBytes(const QByteArray& bytes);
    void setCsvFields(const QStringList& fields);
    void loadProtocolFile(const QString& path);
    void clearProtocol();
    bool startRecording(const QString& path);
    void stopRecording();

signals:
    void chunkReady(QByteArray bytes, bool transmitted, qint64 timestampNs);
    void sourceStateChanged(int state);
    void sourceError(QString message);
    void statisticsChanged(quint64 rxBytes, quint64 txBytes, qsizetype parserBacklog);
    void recordingChanged(bool active);
    void protocolLoaded(QString name, QStringList numericFields);
    void protocolCleared();
    void protocolLoadFailed(QStringList issues);
    void protocolEventsReady(QVariantList events);
    void protocolStatisticsChanged(quint64 decoded,
                                   quint64 discardedBytes,
                                   quint64 checksumErrors,
                                   quint64 lengthErrors,
                                   quint64 decodeErrors);

private slots:
    void drainUiQueue();

private:
    lab::adapters::serial::SerialSource source_;
    lab::adapters::serial::SerialSettings lastSettings_;
    lab::core::TimeSeriesStore timeSeries_{120'000};
    std::mutex uiQueueMutex_;
    std::deque<lab::core::DataChunk> uiQueue_;
    std::mutex protocolQueueMutex_;
    std::deque<lab::core::FrameEvent> protocolQueue_;
    lab::core::ProcessingPipeline processing_{timeSeries_};
    lab::core::RawLogRecorder recorder_;
    QTimer refreshTimer_;
};

}  // namespace lab::app

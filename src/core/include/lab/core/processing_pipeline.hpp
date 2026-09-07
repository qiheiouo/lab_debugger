#pragma once

#include "lab/core/csv_stream_parser.hpp"
#include "lab/core/data_chunk.hpp"
#include "lab/core/frame_stream_parser.hpp"
#include "lab/core/time_series_store.hpp"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <memory>
#include <optional>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <vector>

namespace lab::core {

class ProcessingPipeline {
public:
    using SampleHandler = std::function<void(const DataSample&)>;
    using FrameHandler = std::function<void(const FrameEvent&)>;

    explicit ProcessingPipeline(TimeSeriesStore& store);
    ~ProcessingPipeline();

    ProcessingPipeline(const ProcessingPipeline&) = delete;
    ProcessingPipeline& operator=(const ProcessingPipeline&) = delete;

    void push(DataChunk chunk);
    void setFieldNames(std::vector<std::string> names);
    void setQualifyFieldNames(bool enabled);
    void setSampleHandler(SampleHandler handler);
    void setProtocolDefinition(ProtocolDefinition definition);
    void clearProtocolDefinition();
    void resetParsers();
    void setFrameHandler(FrameHandler handler);
    [[nodiscard]] bool protocolEnabled() const;
    [[nodiscard]] std::optional<FrameParserStatistics> protocolStatistics() const;
    [[nodiscard]] std::size_t pendingChunks() const;
    void flush();

private:
    struct SourceParsers {
        SourceParsers(std::vector<std::string> fieldNames,
                      const std::optional<ProtocolDefinition>& definition);

        CsvStreamParser csv;
        std::unique_ptr<FrameStreamParser> frame;
    };

    void run(std::stop_token stopToken);

    TimeSeriesStore& store_;
    mutable std::mutex queueMutex_;
    std::condition_variable_any queueReady_;
    std::condition_variable queueIdle_;
    std::deque<DataChunk> queue_;
    bool processingChunk_{};

    mutable std::mutex parserMutex_;
    std::vector<std::string> fieldNames_{"field0", "field1", "field2"};
    std::optional<ProtocolDefinition> protocolDefinition_;
    std::unordered_map<std::string, SourceParsers> sourceParsers_;
    bool qualifyFieldNames_{};

    std::mutex handlerMutex_;
    SampleHandler sampleHandler_;
    std::mutex frameHandlerMutex_;
    FrameHandler frameHandler_;
    std::jthread worker_;
};

}  // namespace lab::core

#pragma once

#include "lab/core/csv_stream_parser.hpp"
#include "lab/core/data_chunk.hpp"
#include "lab/core/time_series_store.hpp"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace lab::core {

class ProcessingPipeline {
public:
    using SampleHandler = std::function<void(const DataSample&)>;

    explicit ProcessingPipeline(TimeSeriesStore& store);
    ~ProcessingPipeline();

    ProcessingPipeline(const ProcessingPipeline&) = delete;
    ProcessingPipeline& operator=(const ProcessingPipeline&) = delete;

    void push(DataChunk chunk);
    void setFieldNames(std::vector<std::string> names);
    void setSampleHandler(SampleHandler handler);
    [[nodiscard]] std::size_t pendingChunks() const;

private:
    void run(std::stop_token stopToken);

    TimeSeriesStore& store_;
    mutable std::mutex queueMutex_;
    std::condition_variable_any queueReady_;
    std::deque<DataChunk> queue_;

    std::mutex parserMutex_;
    CsvStreamParser parser_;

    std::mutex handlerMutex_;
    SampleHandler sampleHandler_;
    std::jthread worker_;
};

}  // namespace lab::core


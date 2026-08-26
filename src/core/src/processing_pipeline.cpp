#include "lab/core/processing_pipeline.hpp"

namespace lab::core {

ProcessingPipeline::ProcessingPipeline(TimeSeriesStore& store)
    : store_(store), worker_([this](std::stop_token token) { run(token); }) {}

ProcessingPipeline::~ProcessingPipeline() {
    worker_.request_stop();
    queueReady_.notify_all();
}

void ProcessingPipeline::push(DataChunk chunk) {
    {
        std::scoped_lock lock(queueMutex_);
        queue_.push_back(std::move(chunk));
    }
    queueReady_.notify_one();
}

void ProcessingPipeline::setFieldNames(std::vector<std::string> names) {
    std::scoped_lock lock(parserMutex_);
    parser_.setFieldNames(std::move(names));
    parser_.reset();
}

void ProcessingPipeline::setSampleHandler(SampleHandler handler) {
    std::scoped_lock lock(handlerMutex_);
    sampleHandler_ = std::move(handler);
}

std::size_t ProcessingPipeline::pendingChunks() const {
    std::scoped_lock lock(queueMutex_);
    return queue_.size();
}

void ProcessingPipeline::run(std::stop_token stopToken) {
    while (true) {
        DataChunk chunk;
        {
            std::unique_lock lock(queueMutex_);
            queueReady_.wait(lock, stopToken, [this] { return !queue_.empty(); });
            if (queue_.empty()) {
                if (stopToken.stop_requested()) {
                    break;
                }
                continue;
            }
            chunk = std::move(queue_.front());
            queue_.pop_front();
        }

        if (chunk.direction != Direction::Rx) {
            continue;
        }

        std::vector<DataSample> samples;
        {
            std::scoped_lock lock(parserMutex_);
            samples = parser_.consume(
                chunk.payload,
                chunk.sourceTimestamp,
                chunk.sourceId,
                chunk.sequence);
        }

        for (const auto& sample : samples) {
            store_.append(sample);
            SampleHandler handler;
            {
                std::scoped_lock lock(handlerMutex_);
                handler = sampleHandler_;
            }
            if (handler) {
                handler(sample);
            }
        }
    }
}

}  // namespace lab::core


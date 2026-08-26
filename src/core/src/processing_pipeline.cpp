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

void ProcessingPipeline::setProtocolDefinition(ProtocolDefinition definition) {
    std::scoped_lock lock(parserMutex_, queueMutex_);
    queue_.clear();
    frameParser_ = std::make_unique<FrameStreamParser>(std::move(definition));
    parser_.reset();
    csvEnabled_ = false;
}

void ProcessingPipeline::clearProtocolDefinition() {
    std::scoped_lock lock(parserMutex_, queueMutex_);
    queue_.clear();
    frameParser_.reset();
    parser_.reset();
    csvEnabled_ = true;
}

void ProcessingPipeline::setFrameHandler(FrameHandler handler) {
    std::scoped_lock lock(frameHandlerMutex_);
    frameHandler_ = std::move(handler);
}

bool ProcessingPipeline::protocolEnabled() const {
    std::scoped_lock lock(parserMutex_);
    return frameParser_ != nullptr;
}

std::optional<FrameParserStatistics> ProcessingPipeline::protocolStatistics() const {
    std::scoped_lock lock(parserMutex_);
    if (!frameParser_) {
        return std::nullopt;
    }
    return frameParser_->statistics();
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
        std::vector<FrameEvent> frameEvents;
        {
            std::scoped_lock lock(parserMutex_);
            if (csvEnabled_) {
                samples = parser_.consume(
                    chunk.payload,
                    chunk.sourceTimestamp,
                    chunk.sourceId,
                    chunk.sequence);
            }
            if (frameParser_) {
                frameEvents = frameParser_->consume(chunk);
            }
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

        for (const auto& event : frameEvents) {
            if (event.kind == FrameEventKind::FrameDecoded) {
                for (const auto& field : event.fields) {
                    if (!field.numericValue) {
                        continue;
                    }
                    store_.append({
                        event.sourceTimestamp,
                        event.sourceId,
                        field.name,
                        *field.numericValue,
                        field.unit,
                        event.sequence});
                }
            }
            FrameHandler handler;
            {
                std::scoped_lock lock(frameHandlerMutex_);
                handler = frameHandler_;
            }
            if (handler) {
                handler(event);
            }
        }
    }
}

}  // namespace lab::core

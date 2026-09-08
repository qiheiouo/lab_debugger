#include "lab/core/processing_pipeline.hpp"

#include <limits>

namespace lab::core {
namespace {

void saturatingAdd(std::uint64_t& destination, std::uint64_t value) noexcept {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    destination = value > maximum - destination ? maximum : destination + value;
}

}  // namespace

ProcessingPipeline::SourceParsers::SourceParsers(
    std::vector<std::string> fieldNames,
    const std::optional<ProtocolDefinition>& definition)
    : csv(std::move(fieldNames)) {
    if (definition) {
        frame = std::make_unique<FrameStreamParser>(*definition);
    }
}

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
    fieldNames_ = std::move(names);
    sourceParsers_.clear();
}

void ProcessingPipeline::setQualifyFieldNames(bool enabled) {
    std::scoped_lock lock(parserMutex_, queueMutex_);
    queue_.clear();
    sourceParsers_.clear();
    qualifyFieldNames_ = enabled;
}

void ProcessingPipeline::setSampleHandler(SampleHandler handler) {
    std::scoped_lock lock(handlerMutex_);
    sampleHandler_ = std::move(handler);
}

void ProcessingPipeline::setSampleBatchHandler(SampleBatchHandler handler) {
    std::scoped_lock lock(handlerMutex_);
    sampleBatchHandler_ = std::move(handler);
}

void ProcessingPipeline::setProtocolDefinition(ProtocolDefinition definition) {
    std::scoped_lock lock(parserMutex_, queueMutex_);
    queue_.clear();
    protocolDefinition_ = std::move(definition);
    sourceParsers_.clear();
}

void ProcessingPipeline::clearProtocolDefinition() {
    std::scoped_lock lock(parserMutex_, queueMutex_);
    queue_.clear();
    protocolDefinition_.reset();
    sourceParsers_.clear();
}

void ProcessingPipeline::resetParsers() {
    std::scoped_lock lock(parserMutex_);
    sourceParsers_.clear();
}

void ProcessingPipeline::setFrameHandler(FrameHandler handler) {
    std::scoped_lock lock(frameHandlerMutex_);
    frameHandler_ = std::move(handler);
}

bool ProcessingPipeline::protocolEnabled() const {
    std::scoped_lock lock(parserMutex_);
    return protocolDefinition_.has_value();
}

std::optional<FrameParserStatistics> ProcessingPipeline::protocolStatistics() const {
    std::scoped_lock lock(parserMutex_);
    if (!protocolDefinition_) {
        return std::nullopt;
    }
    FrameParserStatistics result;
    for (const auto& [sourceId, parsers] : sourceParsers_) {
        static_cast<void>(sourceId);
        if (!parsers.frame) {
            continue;
        }
        const auto statistics = parsers.frame->statistics();
        saturatingAdd(result.decodedFrames, statistics.decodedFrames);
        saturatingAdd(result.discardedBytes, statistics.discardedBytes);
        saturatingAdd(result.checksumErrors, statistics.checksumErrors);
        saturatingAdd(result.lengthErrors, statistics.lengthErrors);
        saturatingAdd(result.decodeErrors, statistics.decodeErrors);
    }
    return result;
}

std::size_t ProcessingPipeline::pendingChunks() const {
    std::scoped_lock lock(queueMutex_);
    return queue_.size();
}

void ProcessingPipeline::flush() {
    std::unique_lock lock(queueMutex_);
    queueIdle_.wait(lock, [this] { return queue_.empty() && !processingChunk_; });
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
            processingChunk_ = true;
        }

        if (chunk.direction != Direction::Rx) {
            std::scoped_lock lock(queueMutex_);
            processingChunk_ = false;
            if (queue_.empty()) {
                queueIdle_.notify_all();
            }
            continue;
        }

        std::vector<std::vector<DataSample>> sampleBatches;
        std::vector<FrameEvent> frameEvents;
        bool qualifyFieldNames = false;
        {
            std::scoped_lock lock(parserMutex_);
            qualifyFieldNames = qualifyFieldNames_;
            auto [parsers, inserted] = sourceParsers_.try_emplace(
                chunk.sourceId, fieldNames_, protocolDefinition_);
            static_cast<void>(inserted);
            if (!protocolDefinition_) {
                sampleBatches = parsers->second.csv.consumeBatches(
                    chunk.payload,
                    chunk.sourceTimestamp,
                    chunk.sourceId,
                    chunk.sequence);
                if (qualifyFieldNames) {
                    for (auto& batch : sampleBatches) {
                        for (auto& sample : batch) {
                            sample.field = chunk.sourceId + "." + sample.field;
                        }
                    }
                }
            }
            if (parsers->second.frame) {
                frameEvents = parsers->second.frame->consume(chunk);
            }
        }

        for (const auto& batch : sampleBatches) {
            SampleHandler handler;
            SampleBatchHandler batchHandler;
            {
                std::scoped_lock lock(handlerMutex_);
                handler = sampleHandler_;
                batchHandler = sampleBatchHandler_;
            }
            for (const auto& sample : batch) {
                store_.append(sample);
                if (handler) handler(sample);
            }
            if (batchHandler) batchHandler(std::span(batch));
        }

        for (const auto& event : frameEvents) {
            std::vector<DataSample> decodedSamples;
            if (event.kind == FrameEventKind::FrameDecoded) {
                decodedSamples.reserve(event.fields.size());
                for (const auto& field : event.fields) {
                    if (!field.numericValue) {
                        continue;
                    }
                    decodedSamples.push_back({
                        event.sourceTimestamp,
                        event.sourceId,
                        qualifyFieldNames ? event.sourceId + "." + field.name
                                          : field.name,
                        *field.numericValue,
                        field.unit,
                        event.sequence});
                    const auto& sample = decodedSamples.back();
                    store_.append(sample);
                    SampleHandler sampleHandler;
                    {
                        std::scoped_lock lock(handlerMutex_);
                        sampleHandler = sampleHandler_;
                    }
                    if (sampleHandler) {
                        sampleHandler(sample);
                    }
                }
                SampleBatchHandler batchHandler;
                {
                    std::scoped_lock lock(handlerMutex_);
                    batchHandler = sampleBatchHandler_;
                }
                if (batchHandler && !decodedSamples.empty()) {
                    batchHandler(std::span(decodedSamples));
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

        {
            std::scoped_lock lock(queueMutex_);
            processingChunk_ = false;
            if (queue_.empty()) {
                queueIdle_.notify_all();
            }
        }
    }
}

}  // namespace lab::core

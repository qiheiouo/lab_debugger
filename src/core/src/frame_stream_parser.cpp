#include "lab/core/frame_stream_parser.hpp"

#include "lab/core/checksum.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace lab::core {
namespace {

std::uint64_t readLengthValue(
    std::span<const std::uint8_t> bytes,
    Endian endian) noexcept {
    std::uint64_t value = 0;
    if (endian == Endian::Little) {
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
        }
    } else {
        for (const auto byte : bytes) {
            value = (value << 8U) | byte;
        }
    }
    return value;
}

std::size_t partialHeaderSuffix(
    const std::vector<std::uint8_t>& buffer,
    const std::vector<std::uint8_t>& header) {
    const auto limit = std::min(buffer.size(), header.size() - 1);
    for (std::size_t length = limit; length > 0; --length) {
        if (std::equal(buffer.end() - static_cast<std::ptrdiff_t>(length),
                       buffer.end(), header.begin())) {
            return length;
        }
    }
    return 0;
}

}  // namespace

FrameStreamParser::FrameStreamParser(ProtocolDefinition definition) {
    setDefinition(std::move(definition));
}

void FrameStreamParser::setDefinition(ProtocolDefinition definition) {
    const auto issues = validateProtocol(definition);
    if (!issues.empty()) {
        throw std::invalid_argument(issues.front().message);
    }
    definition_ = std::move(definition);
    reset();
}

void FrameStreamParser::reset() {
    buffer_.clear();
    segments_.clear();
    statistics_ = {};
}

std::vector<FrameEvent> FrameStreamParser::consume(const DataChunk& chunk) {
    std::vector<FrameEvent> events;
    if (chunk.direction != Direction::Rx || chunk.payload.empty()) {
        return events;
    }
    const auto safetyLimit = definition_.maximumFrameLength * 4U;
    if (chunk.payload.size() > safetyLimit) {
        if (!buffer_.empty()) {
            discardGarbage(buffer_.size(), events, "Parser buffer safety limit reached");
        }
        events.push_back({
            FrameEventKind::GarbageDiscarded,
            chunk.sourceId,
            chunk.sourceTimestamp,
            chunk.receiveTimestamp,
            chunk.sequence,
            chunk.payload,
            {},
            "Input chunk exceeds parser safety limit"});
        statistics_.discardedBytes += chunk.payload.size();
        return events;
    }
    if (buffer_.size() > safetyLimit - chunk.payload.size()) {
        discardGarbage(buffer_.size(), events, "Parser buffer safety limit reached");
    }
    buffer_.insert(buffer_.end(), chunk.payload.begin(), chunk.payload.end());
    segments_.push_back({
        chunk.payload.size(),
        chunk.sourceId,
        chunk.sourceTimestamp,
        chunk.receiveTimestamp,
        chunk.sequence});

    while (!buffer_.empty()) {
        const auto header = std::search(
            buffer_.begin(), buffer_.end(),
            definition_.header.begin(), definition_.header.end());
        if (header == buffer_.end()) {
            const auto keep = partialHeaderSuffix(buffer_, definition_.header);
            discardGarbage(
                buffer_.size() - keep,
                events,
                "Bytes did not match frame header");
            break;
        }
        const auto prefix = static_cast<std::size_t>(std::distance(buffer_.begin(), header));
        if (prefix > 0) {
            discardGarbage(prefix, events, "Discarded bytes before frame header");
            continue;
        }
        if (buffer_.size() < definition_.header.size()) {
            break;
        }

        std::string lengthError;
        const auto frameLength = determineFrameLength(lengthError);
        if (!frameLength) {
            if (lengthError.empty()) {
                break;
            }
            const auto capture = std::min(buffer_.size(), definition_.header.size() + 8U);
            const auto context = frontContext();
            events.push_back({
                FrameEventKind::LengthError,
                context.sourceId,
                context.sourceTimestamp,
                context.receiveTimestamp,
                context.sequence,
                std::vector<std::uint8_t>(buffer_.begin(), buffer_.begin() +
                    static_cast<std::ptrdiff_t>(capture)),
                {},
                lengthError});
            ++statistics_.lengthErrors;
            consumeBufferedBytes(1);
            continue;
        }
        if (buffer_.size() < *frameLength) {
            break;
        }

        std::vector<std::uint8_t> frame(
            buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(*frameLength));
        const auto context = frontContext();
        std::string checksumError;
        if (!verifyFrameChecksum(frame, definition_.checksum, &checksumError)) {
            events.push_back({
                FrameEventKind::ChecksumError,
                context.sourceId,
                context.sourceTimestamp,
                context.receiveTimestamp,
                context.sequence,
                frame,
                {},
                std::move(checksumError)});
            ++statistics_.checksumErrors;
            consumeBufferedBytes(1);
            continue;
        }

        auto decoded = decodeProtocolFrame(frame, definition_);
        if (!decoded.success) {
            events.push_back({
                FrameEventKind::DecodeError,
                context.sourceId,
                context.sourceTimestamp,
                context.receiveTimestamp,
                context.sequence,
                frame,
                {},
                std::move(decoded.error)});
            ++statistics_.decodeErrors;
        } else {
            events.push_back({
                FrameEventKind::FrameDecoded,
                context.sourceId,
                context.sourceTimestamp,
                context.receiveTimestamp,
                context.sequence,
                frame,
                std::move(decoded.fields),
                {}});
            ++statistics_.decodedFrames;
        }
        consumeBufferedBytes(*frameLength);
    }
    return events;
}

const ProtocolDefinition& FrameStreamParser::definition() const noexcept {
    return definition_;
}

FrameParserStatistics FrameStreamParser::statistics() const noexcept {
    return statistics_;
}

std::size_t FrameStreamParser::bufferedBytes() const noexcept {
    return buffer_.size();
}

std::optional<std::size_t> FrameStreamParser::determineFrameLength(
    std::string& error) const {
    if (definition_.fixedFrameLength) {
        return *definition_.fixedFrameLength;
    }
    const auto& length = *definition_.lengthField;
    const auto width = fieldTypeByteSize(length.type);
    if (buffer_.size() < length.byteOffset + width) {
        return std::nullopt;
    }
    const auto raw = readLengthValue(
        std::span(buffer_).subspan(length.byteOffset, width), length.endian);
    if (raw > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        error = "Length field value is too large";
        return std::nullopt;
    }
    const auto signedRaw = static_cast<std::int64_t>(raw);
    if ((length.adjustment > 0 &&
         signedRaw > std::numeric_limits<std::int64_t>::max() - length.adjustment) ||
        (length.adjustment < 0 &&
         signedRaw < std::numeric_limits<std::int64_t>::min() - length.adjustment)) {
        error = "Adjusted frame length overflows int64";
        return std::nullopt;
    }
    const auto signedLength = signedRaw + length.adjustment;
    if (signedLength <= 0 ||
        static_cast<std::uint64_t>(signedLength) > definition_.maximumFrameLength) {
        error = "Frame length is outside configured limits";
        return std::nullopt;
    }
    const auto result = static_cast<std::size_t>(signedLength);
    if (result < minimumFrameLength()) {
        error = "Frame length is shorter than required fields";
        return std::nullopt;
    }
    return result;
}

std::size_t FrameStreamParser::minimumFrameLength() const noexcept {
    auto minimum = definition_.header.size();
    if (definition_.lengthField) {
        minimum = std::max(
            minimum,
            definition_.lengthField->byteOffset +
                fieldTypeByteSize(definition_.lengthField->type));
    }
    for (const auto& field : definition_.fields) {
        minimum = std::max(minimum, field.byteOffset + fieldByteSize(field));
    }
    const auto checksumWidth = checksumByteSize(definition_.checksum.type);
    if (checksumWidth > 0) {
        minimum = definition_.checksum.byteOffset
                      ? std::max(minimum, *definition_.checksum.byteOffset + checksumWidth)
                      : minimum + checksumWidth;
    }
    return minimum;
}

void FrameStreamParser::discardGarbage(
    std::size_t count,
    std::vector<FrameEvent>& events,
    std::string message) {
    if (count == 0) {
        return;
    }
    const auto context = frontContext();
    events.push_back({
        FrameEventKind::GarbageDiscarded,
        context.sourceId,
        context.sourceTimestamp,
        context.receiveTimestamp,
        context.sequence,
        std::vector<std::uint8_t>(
            buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(count)),
        {},
        std::move(message)});
    statistics_.discardedBytes += count;
    consumeBufferedBytes(count);
}

DataChunk FrameStreamParser::frontContext() const {
    if (segments_.empty()) {
        return {};
    }
    const auto& segment = segments_.front();
    return {
        segment.sourceId,
        segment.sourceTimestamp,
        segment.receiveTimestamp,
        segment.sequence,
        Direction::Rx,
        {}};
}

void FrameStreamParser::consumeBufferedBytes(std::size_t count) {
    count = std::min(count, buffer_.size());
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(count));
    while (count > 0 && !segments_.empty()) {
        if (count < segments_.front().count) {
            segments_.front().count -= count;
            count = 0;
        } else {
            count -= segments_.front().count;
            segments_.pop_front();
        }
    }
}

}  // namespace lab::core

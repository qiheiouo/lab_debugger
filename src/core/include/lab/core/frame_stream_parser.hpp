#pragma once

#include "lab/core/data_chunk.hpp"
#include "lab/core/protocol_decoder.hpp"
#include "lab/core/protocol_definition.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace lab::core {

enum class FrameEventKind {
    FrameDecoded,
    GarbageDiscarded,
    ChecksumError,
    LengthError,
    DecodeError
};

struct FrameEvent {
    FrameEventKind kind{FrameEventKind::FrameDecoded};
    std::string sourceId;
    Timestamp sourceTimestamp{};
    Timestamp receiveTimestamp{};
    std::uint64_t sequence{};
    std::vector<std::uint8_t> rawBytes;
    std::vector<DecodedField> fields;
    std::string message;
};

struct FrameParserStatistics {
    std::uint64_t decodedFrames{};
    std::uint64_t discardedBytes{};
    std::uint64_t checksumErrors{};
    std::uint64_t lengthErrors{};
    std::uint64_t decodeErrors{};
};

class FrameStreamParser {
public:
    explicit FrameStreamParser(ProtocolDefinition definition);

    void setDefinition(ProtocolDefinition definition);
    void reset();

    [[nodiscard]] std::vector<FrameEvent> consume(const DataChunk& chunk);
    [[nodiscard]] const ProtocolDefinition& definition() const noexcept;
    [[nodiscard]] FrameParserStatistics statistics() const noexcept;
    [[nodiscard]] std::size_t bufferedBytes() const noexcept;

private:
    struct BufferSegment {
        std::size_t count{};
        std::string sourceId;
        Timestamp sourceTimestamp{};
        Timestamp receiveTimestamp{};
        std::uint64_t sequence{};
    };

    [[nodiscard]] std::optional<std::size_t> determineFrameLength(
        std::string& error) const;
    [[nodiscard]] std::size_t minimumFrameLength() const noexcept;
    void discardGarbage(
        std::size_t count,
        std::vector<FrameEvent>& events,
        std::string message);
    [[nodiscard]] DataChunk frontContext() const;
    void consumeBufferedBytes(std::size_t count);

    ProtocolDefinition definition_;
    std::vector<std::uint8_t> buffer_;
    std::deque<BufferSegment> segments_;
    FrameParserStatistics statistics_;
};

}  // namespace lab::core

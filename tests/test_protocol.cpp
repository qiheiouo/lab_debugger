#include "lab/core/checksum.hpp"
#include "lab/core/frame_stream_parser.hpp"
#include "lab/core/protocol_decoder.hpp"
#include "lab/core/protocol_json_loader.hpp"

#include <bit>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <random>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error("protocol: " + message);
    }
}

std::vector<std::uint8_t> fixedFrame() {
    std::vector<std::uint8_t> frame{
        0xAA, 0x55,
        0x00, 0x00, 0xC0, 0x3F,
        0x60, 0x09,
        0x02,
        0x01};
    const auto crc = lab::core::crc16Modbus(frame);
    frame.push_back(static_cast<std::uint8_t>(crc & 0xFFU));
    frame.push_back(static_cast<std::uint8_t>(crc >> 8U));
    return frame;
}

std::string fixedProtocolJson() {
    return R"json({
        "name": "stm32_status",
        "frame": {
            "header": ["0xAA", "0x55"],
            "length": 12
        },
        "fields": [
            {"name":"speed", "type":"float32", "unit":"m/s"},
            {"name":"voltage", "type":"uint16", "scale":0.01, "unit":"V"},
            {"name":"mode", "type":"uint8", "enum":{"0":"idle", "2":"run"}},
            {"name":"ready", "type":"bool"}
        ],
        "checksum": {
            "type":"crc16_modbus",
            "offset":10,
            "range_start":0,
            "range_length":10,
            "endian":"little"
        }
    })json";
}

lab::core::DataChunk chunk(
    std::vector<std::uint8_t> bytes,
    std::uint64_t sequence = 1,
    lab::core::Timestamp timestamp = 100) {
    return {"mock", timestamp, timestamp + 1, sequence,
            lab::core::Direction::Rx, std::move(bytes)};
}

std::size_t countKind(
    const std::vector<lab::core::FrameEvent>& events,
    lab::core::FrameEventKind kind) {
    std::size_t count = 0;
    for (const auto& event : events) {
        if (event.kind == kind) ++count;
    }
    return count;
}

const lab::core::FrameEvent* firstDecoded(
    const std::vector<lab::core::FrameEvent>& events) {
    for (const auto& event : events) {
        if (event.kind == lab::core::FrameEventKind::FrameDecoded) return &event;
    }
    return nullptr;
}

void testChecksumVectors() {
    const std::string input = "123456789";
    const auto bytes = std::span(
        reinterpret_cast<const std::uint8_t*>(input.data()), input.size());
    require(lab::core::checksumSum8(bytes) == 0xDD, "sum8 reference vector");
    require(lab::core::crc8Atm(bytes) == 0xF4, "CRC-8/ATM reference vector");
    require(lab::core::crc16Modbus(bytes) == 0x4B37, "CRC-16/MODBUS reference vector");
    require(lab::core::crc16CcittFalse(bytes) == 0x29B1,
            "CRC-16/CCITT-FALSE reference vector");
}

void testJsonLoadAndFieldDecode() {
    const auto loaded = lab::core::loadProtocolJson(fixedProtocolJson());
    require(loaded.success(), "valid JSON protocol loads");
    require(loaded.definition->fields.size() == 4, "field count");
    const auto decoded = lab::core::decodeProtocolFrame(fixedFrame(), *loaded.definition);
    require(decoded.success, "fixed frame decodes");
    require(std::abs(*decoded.fields[0].numericValue - 1.5) < 1e-6, "float32 decode");
    require(std::abs(*decoded.fields[1].numericValue - 24.0) < 1e-9,
            "uint16 scale and unit");
    require(decoded.fields[2].enumLabel == "run", "enum label");
    require(std::get<bool>(decoded.fields[3].rawValue), "boolean decode");
}

void testHalfFramesStickyPacketsAndGarbage() {
    const auto loaded = lab::core::loadProtocolJson(fixedProtocolJson());
    lab::core::FrameStreamParser parser(*loaded.definition);
    const auto frame = fixedFrame();

    auto events = parser.consume(chunk({frame.begin(), frame.begin() + 5}));
    require(events.empty() && parser.bufferedBytes() == 5, "half frame buffered");
    events = parser.consume(chunk({frame.begin() + 5, frame.end()}, 2, 200));
    require(countKind(events, lab::core::FrameEventKind::FrameDecoded) == 1,
            "second half completes frame");
    const auto* splitFrame = firstDecoded(events);
    require(splitFrame && splitFrame->sourceTimestamp == 100 && splitFrame->sequence == 1,
            "frame keeps timestamp of its first byte");

    parser.reset();
    std::vector<std::uint8_t> stream{0x00, 0x13, 0x37};
    stream.insert(stream.end(), frame.begin(), frame.end());
    stream.insert(stream.end(), frame.begin(), frame.end());
    events = parser.consume(chunk(stream));
    require(countKind(events, lab::core::FrameEventKind::GarbageDiscarded) == 1,
            "garbage prefix reported");
    require(countKind(events, lab::core::FrameEventKind::FrameDecoded) == 2,
            "sticky frames split");
    require(parser.statistics().decodedFrames == 2 &&
                parser.statistics().discardedBytes == 3,
            "parser statistics");

    parser.reset();
    events = parser.consume(chunk({0x42, 0xAA}));
    require(parser.bufferedBytes() == 1, "partial header suffix retained");
    std::vector<std::uint8_t> tail(frame.begin() + 1, frame.end());
    events = parser.consume(chunk(tail, 2));
    require(countKind(events, lab::core::FrameEventKind::FrameDecoded) == 1,
            "header split across chunks");
}

void testCrcErrorAndResynchronization() {
    const auto loaded = lab::core::loadProtocolJson(fixedProtocolJson());
    lab::core::FrameStreamParser parser(*loaded.definition);
    auto damaged = fixedFrame();
    damaged[6] ^= 0x40;
    auto valid = fixedFrame();
    damaged.insert(damaged.end(), valid.begin(), valid.end());
    const auto events = parser.consume(chunk(damaged));
    require(countKind(events, lab::core::FrameEventKind::ChecksumError) == 1,
            "CRC error reported");
    require(countKind(events, lab::core::FrameEventKind::FrameDecoded) == 1,
            "parser resynchronizes after CRC error");
}

void testRandomChunkBoundaries() {
    const auto loaded = lab::core::loadProtocolJson(fixedProtocolJson());
    lab::core::FrameStreamParser parser(*loaded.definition);
    const auto frame = fixedFrame();
    std::vector<std::uint8_t> stream;
    for (int index = 0; index < 100; ++index) {
        if (index % 9 == 0) {
            stream.push_back(0x00);
            stream.push_back(0x7E);
        }
        stream.insert(stream.end(), frame.begin(), frame.end());
    }

    std::mt19937 random(0x1A2B3C4D);
    std::size_t offset = 0;
    std::size_t decoded = 0;
    std::uint64_t sequence = 1;
    while (offset < stream.size()) {
        const auto count = std::min<std::size_t>(
            1 + random() % 31, stream.size() - offset);
        auto events = parser.consume(chunk(
            std::vector<std::uint8_t>(stream.begin() + static_cast<std::ptrdiff_t>(offset),
                                      stream.begin() + static_cast<std::ptrdiff_t>(offset + count)),
            sequence,
            static_cast<lab::core::Timestamp>(sequence * 1'000)));
        decoded += countKind(events, lab::core::FrameEventKind::FrameDecoded);
        offset += count;
        ++sequence;
    }
    require(decoded == 100, "random chunk boundaries preserve every valid frame");
    require(parser.bufferedBytes() == 0, "no residual bytes after randomized stream");
}

void testDynamicLengthAndLengthError() {
    const std::string json = R"json({
        "name":"dynamic",
        "maximum_frame_length":64,
        "frame":{
            "header":[171,205],
            "length_field":{"offset":2,"type":"uint8"}
        },
        "fields":[
            {"name":"value","type":"uint16","byte_offset":3,"endian":"big"}
        ],
        "checksum":{"type":"sum8","offset":5,"range_length":5}
    })json";
    const auto loaded = lab::core::loadProtocolJson(json);
    require(loaded.success(), "dynamic protocol loads");
    lab::core::FrameStreamParser parser(*loaded.definition);

    std::vector<std::uint8_t> valid{0xAB, 0xCD, 0x06, 0x12, 0x34};
    valid.push_back(lab::core::checksumSum8(valid));
    std::vector<std::uint8_t> stream{0xAB, 0xCD, 0x02};
    stream.insert(stream.end(), valid.begin(), valid.end());
    const auto events = parser.consume(chunk(stream));
    require(countKind(events, lab::core::FrameEventKind::LengthError) == 1,
            "invalid dynamic length reported");
    const auto* decoded = firstDecoded(events);
    require(decoded && *decoded->fields[0].numericValue == 0x1234,
            "dynamic frame resynchronized and big-endian field decoded");
}

void testAllFieldTypesAndByteArray() {
    lab::core::ProtocolDefinition definition;
    definition.name = "types";
    definition.header = {0xF0};
    definition.fixedFrameLength = 22;
    definition.fields = {
        {"i8", lab::core::FieldType::Int8, 1},
        {"i16", lab::core::FieldType::Int16, 2, 0, lab::core::Endian::Big},
        {"u32", lab::core::FieldType::UInt32, 4, 0, lab::core::Endian::Big},
        {"i32", lab::core::FieldType::Int32, 8},
        {"f64", lab::core::FieldType::Float64, 12},
        {"flag", lab::core::FieldType::Boolean, 20},
        {"raw", lab::core::FieldType::ByteArray, 21, 1}};
    require(lab::core::validateProtocol(definition).empty(), "programmatic definition valid");

    std::vector<std::uint8_t> frame(22, 0);
    frame[0] = 0xF0;
    frame[1] = 0xFF;
    frame[2] = 0xFF;
    frame[3] = 0xFE;
    frame[4] = 0x12;
    frame[5] = 0x34;
    frame[6] = 0x56;
    frame[7] = 0x78;
    frame[8] = 0xFE;
    frame[9] = 0xFF;
    frame[10] = 0xFF;
    frame[11] = 0xFF;
    const auto doubleBits = std::bit_cast<std::uint64_t>(3.25);
    for (int index = 0; index < 8; ++index) {
        frame[12 + index] = static_cast<std::uint8_t>(doubleBits >> (index * 8));
    }
    frame[20] = 1;
    frame[21] = 0xA5;
    const auto decoded = lab::core::decodeProtocolFrame(frame, definition);
    require(decoded.success, "all field types decode");
    require(std::get<std::int64_t>(decoded.fields[0].rawValue) == -1, "int8 sign");
    require(std::get<std::int64_t>(decoded.fields[1].rawValue) == -2, "int16 sign big endian");
    require(std::get<std::uint64_t>(decoded.fields[2].rawValue) == 0x12345678,
            "uint32 big endian");
    require(std::get<std::int64_t>(decoded.fields[3].rawValue) == -2, "int32 sign");
    require(std::abs(*decoded.fields[4].numericValue - 3.25) < 1e-12, "float64");
    require(std::get<std::vector<std::uint8_t>>(decoded.fields[6].rawValue)[0] == 0xA5,
            "byte array");
}

void testInvalidDefinitions() {
    auto loaded = lab::core::loadProtocolJson("{ invalid json }");
    require(!loaded.success() && loaded.issues[0].code == "json.syntax", "syntax error");

    loaded = lab::core::loadProtocolJson(R"json({
        "name":"invalid",
        "frame":{"header":[170],"length":3},
        "fields":[{"name":"x","type":"not_a_type"}]
    })json");
    require(!loaded.success(), "unknown field type rejected");

    lab::core::ProtocolDefinition overlap;
    overlap.name = "overlap";
    overlap.header = {0xAA};
    overlap.fixedFrameLength = 5;
    overlap.fields = {
        {"a", lab::core::FieldType::UInt32, 1},
        {"b", lab::core::FieldType::UInt16, 2}};
    const auto issues = lab::core::validateProtocol(overlap);
    require(!issues.empty(), "overlapping fields rejected");
}

}  // namespace

void runProtocolTests() {
    testChecksumVectors();
    testJsonLoadAndFieldDecode();
    testHalfFramesStickyPacketsAndGarbage();
    testCrcErrorAndResynchronization();
    testRandomChunkBoundaries();
    testDynamicLengthAndLengthError();
    testAllFieldTypesAndByteArray();
    testInvalidDefinitions();
}

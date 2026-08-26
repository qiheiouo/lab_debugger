#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace lab::core {

enum class FieldType {
    UInt8,
    Int8,
    UInt16,
    Int16,
    UInt32,
    Int32,
    Float32,
    Float64,
    Boolean,
    ByteArray
};

enum class Endian { Little, Big };

enum class ChecksumType {
    None,
    Sum8,
    Crc8Atm,
    Crc16Modbus,
    Crc16CcittFalse
};

struct FieldDefinition {
    std::string name;
    FieldType type{FieldType::UInt8};
    std::size_t byteOffset{};
    std::size_t arrayLength{};
    Endian endian{Endian::Little};
    double scale{1.0};
    double valueOffset{};
    std::string unit;
    std::map<std::int64_t, std::string> enumValues;
};

struct LengthFieldDefinition {
    std::size_t byteOffset{};
    FieldType type{FieldType::UInt16};
    Endian endian{Endian::Little};
    std::int64_t adjustment{};
};

struct ChecksumDefinition {
    ChecksumType type{ChecksumType::None};
    std::optional<std::size_t> byteOffset;
    std::size_t rangeStart{};
    std::optional<std::size_t> rangeLength;
    Endian endian{Endian::Little};
};

struct ProtocolDefinition {
    std::string name;
    std::vector<std::uint8_t> header;
    std::optional<std::size_t> fixedFrameLength;
    std::optional<LengthFieldDefinition> lengthField;
    std::vector<FieldDefinition> fields;
    ChecksumDefinition checksum;
    std::size_t maximumFrameLength{1024 * 1024};
};

struct ProtocolIssue {
    std::string code;
    std::string message;
};

[[nodiscard]] std::size_t fieldByteSize(const FieldDefinition& field) noexcept;
[[nodiscard]] std::size_t fieldTypeByteSize(FieldType type) noexcept;
[[nodiscard]] bool isIntegerFieldType(FieldType type) noexcept;
[[nodiscard]] bool isUnsignedIntegerFieldType(FieldType type) noexcept;
[[nodiscard]] std::size_t checksumByteSize(ChecksumType type) noexcept;
[[nodiscard]] std::vector<ProtocolIssue> validateProtocol(
    const ProtocolDefinition& definition);

[[nodiscard]] std::string toString(FieldType type);
[[nodiscard]] std::string toString(Endian endian);
[[nodiscard]] std::string toString(ChecksumType type);

}  // namespace lab::core


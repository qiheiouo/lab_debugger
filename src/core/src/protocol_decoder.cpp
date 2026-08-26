#include "lab/core/protocol_decoder.hpp"

#include <bit>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <type_traits>

namespace lab::core {
namespace {

std::uint64_t readUnsigned(
    std::span<const std::uint8_t> bytes,
    Endian endian) noexcept {
    std::uint64_t result = 0;
    if (endian == Endian::Little) {
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            result |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
        }
    } else {
        for (const auto byte : bytes) {
            result = (result << 8U) | byte;
        }
    }
    return result;
}

std::string bytesToHex(const std::vector<std::uint8_t>& bytes) {
    std::ostringstream stream;
    stream << std::hex << std::uppercase << std::setfill('0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index > 0) stream << ' ';
        stream << std::setw(2) << static_cast<unsigned>(bytes[index]);
    }
    return stream.str();
}

}  // namespace

DecodeResult decodeProtocolFrame(
    std::span<const std::uint8_t> frame,
    const ProtocolDefinition& definition) {
    DecodeResult result;
    result.fields.reserve(definition.fields.size());

    for (const auto& field : definition.fields) {
        const auto width = fieldByteSize(field);
        if (field.byteOffset > frame.size() || width > frame.size() - field.byteOffset) {
            result.error = "Field lies outside frame: " + field.name;
            return result;
        }
        const auto bytes = frame.subspan(field.byteOffset, width);
        DecodedField decoded;
        decoded.name = field.name;
        decoded.type = field.type;
        decoded.unit = field.unit;

        std::optional<std::int64_t> signedEnumValue;
        switch (field.type) {
        case FieldType::UInt8:
        case FieldType::UInt16:
        case FieldType::UInt32: {
            const auto value = readUnsigned(bytes, field.endian);
            decoded.rawValue = value;
            decoded.numericValue = static_cast<double>(value) * field.scale + field.valueOffset;
            if (value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                signedEnumValue = static_cast<std::int64_t>(value);
            }
            break;
        }
        case FieldType::Int8: {
            const auto value = std::bit_cast<std::int8_t>(bytes[0]);
            decoded.rawValue = static_cast<std::int64_t>(value);
            decoded.numericValue = static_cast<double>(value) * field.scale + field.valueOffset;
            signedEnumValue = value;
            break;
        }
        case FieldType::Int16: {
            const auto raw = static_cast<std::uint16_t>(readUnsigned(bytes, field.endian));
            const auto value = std::bit_cast<std::int16_t>(raw);
            decoded.rawValue = static_cast<std::int64_t>(value);
            decoded.numericValue = static_cast<double>(value) * field.scale + field.valueOffset;
            signedEnumValue = value;
            break;
        }
        case FieldType::Int32: {
            const auto raw = static_cast<std::uint32_t>(readUnsigned(bytes, field.endian));
            const auto value = std::bit_cast<std::int32_t>(raw);
            decoded.rawValue = static_cast<std::int64_t>(value);
            decoded.numericValue = static_cast<double>(value) * field.scale + field.valueOffset;
            signedEnumValue = value;
            break;
        }
        case FieldType::Float32: {
            const auto raw = static_cast<std::uint32_t>(readUnsigned(bytes, field.endian));
            const auto value = std::bit_cast<float>(raw);
            if (!std::isfinite(value)) {
                result.error = "Non-finite float32 field: " + field.name;
                return result;
            }
            decoded.rawValue = static_cast<double>(value);
            decoded.numericValue = static_cast<double>(value) * field.scale + field.valueOffset;
            break;
        }
        case FieldType::Float64: {
            const auto raw = readUnsigned(bytes, field.endian);
            const auto value = std::bit_cast<double>(raw);
            if (!std::isfinite(value)) {
                result.error = "Non-finite float64 field: " + field.name;
                return result;
            }
            decoded.rawValue = value;
            decoded.numericValue = value * field.scale + field.valueOffset;
            break;
        }
        case FieldType::Boolean: {
            const auto value = bytes[0] != 0;
            decoded.rawValue = value;
            decoded.numericValue = value ? 1.0 : 0.0;
            signedEnumValue = value ? 1 : 0;
            break;
        }
        case FieldType::ByteArray:
            decoded.rawValue = std::vector<std::uint8_t>(bytes.begin(), bytes.end());
            break;
        }

        if (signedEnumValue) {
            const auto iterator = field.enumValues.find(*signedEnumValue);
            if (iterator != field.enumValues.end()) {
                decoded.enumLabel = iterator->second;
            }
        }
        result.fields.push_back(std::move(decoded));
    }

    result.success = true;
    return result;
}

std::string decodedValueToString(const DecodedField& field) {
    if (!field.enumLabel.empty()) {
        return field.enumLabel;
    }
    if (field.numericValue) {
        std::ostringstream stream;
        stream << std::setprecision(10) << *field.numericValue;
        if (!field.unit.empty()) {
            stream << ' ' << field.unit;
        }
        return stream.str();
    }
    return std::visit(
        [](const auto& value) -> std::string {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, bool>) {
                return value ? "true" : "false";
            } else if constexpr (std::is_same_v<T, std::vector<std::uint8_t>>) {
                return bytesToHex(value);
            } else {
                return std::to_string(value);
            }
        },
        field.rawValue);
}

}  // namespace lab::core


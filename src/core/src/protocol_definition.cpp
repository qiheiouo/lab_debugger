#include "lab/core/protocol_definition.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace lab::core {

std::size_t fieldTypeByteSize(FieldType type) noexcept {
    switch (type) {
    case FieldType::UInt8:
    case FieldType::Int8:
    case FieldType::Boolean:
        return 1;
    case FieldType::UInt16:
    case FieldType::Int16:
        return 2;
    case FieldType::UInt32:
    case FieldType::Int32:
    case FieldType::Float32:
        return 4;
    case FieldType::Float64:
        return 8;
    case FieldType::ByteArray:
        return 0;
    }
    return 0;
}

std::size_t fieldByteSize(const FieldDefinition& field) noexcept {
    return field.type == FieldType::ByteArray ? field.arrayLength
                                               : fieldTypeByteSize(field.type);
}

bool isIntegerFieldType(FieldType type) noexcept {
    switch (type) {
    case FieldType::UInt8:
    case FieldType::Int8:
    case FieldType::UInt16:
    case FieldType::Int16:
    case FieldType::UInt32:
    case FieldType::Int32:
        return true;
    default:
        return false;
    }
}

bool isUnsignedIntegerFieldType(FieldType type) noexcept {
    return type == FieldType::UInt8 || type == FieldType::UInt16 ||
           type == FieldType::UInt32;
}

std::size_t checksumByteSize(ChecksumType type) noexcept {
    switch (type) {
    case ChecksumType::Sum8:
    case ChecksumType::Crc8Atm:
        return 1;
    case ChecksumType::Crc16Modbus:
    case ChecksumType::Crc16CcittFalse:
        return 2;
    case ChecksumType::None:
        return 0;
    }
    return 0;
}

std::vector<ProtocolIssue> validateProtocol(
    const ProtocolDefinition& definition) {
    std::vector<ProtocolIssue> issues;
    const auto add = [&issues](std::string code, std::string message) {
        issues.push_back({std::move(code), std::move(message)});
    };

    if (definition.name.empty()) {
        add("protocol.name.empty", "Protocol name must not be empty");
    }
    if (definition.header.empty()) {
        add("frame.header.empty", "A stream protocol requires a non-empty frame header");
    }
    if (definition.header.size() > 64) {
        add("frame.header.too_long", "Frame header must not exceed 64 bytes");
    }
    if (definition.fixedFrameLength.has_value() == definition.lengthField.has_value()) {
        add("frame.length.mode", "Define exactly one of fixed length or length field");
    }
    if (definition.maximumFrameLength == 0) {
        add("frame.maximum.zero", "Maximum frame length must be positive");
    } else if (definition.maximumFrameLength > 64U * 1024U * 1024U) {
        add("frame.maximum.too_large", "Maximum frame length must not exceed 64 MiB");
    }
    if (definition.fixedFrameLength) {
        if (*definition.fixedFrameLength < definition.header.size()) {
            add("frame.length.header", "Fixed frame length is shorter than the header");
        }
        if (*definition.fixedFrameLength > definition.maximumFrameLength) {
            add("frame.length.maximum", "Fixed frame length exceeds maximumFrameLength");
        }
    }
    if (definition.lengthField) {
        if (!isUnsignedIntegerFieldType(definition.lengthField->type)) {
            add("frame.length.type", "Length field must be uint8, uint16, or uint32");
        }
        const auto width = fieldTypeByteSize(definition.lengthField->type);
        if (definition.lengthField->byteOffset + width > definition.maximumFrameLength) {
            add("frame.length.offset", "Length field lies outside maximumFrameLength");
        }
    }

    std::set<std::string> names;
    struct Range {
        std::size_t begin;
        std::size_t end;
        std::string name;
    };
    std::vector<Range> ranges;
    for (const auto& field : definition.fields) {
        if (field.name.empty()) {
            add("field.name.empty", "Field name must not be empty");
        } else if (!names.insert(field.name).second) {
            add("field.name.duplicate", "Duplicate field name: " + field.name);
        }
        const auto width = fieldByteSize(field);
        if (width == 0) {
            add("field.size.zero", "Field has zero width: " + field.name);
            continue;
        }
        if (!std::isfinite(field.scale) || !std::isfinite(field.valueOffset)) {
            add("field.transform.invalid", "Field scale and value offset must be finite: " + field.name);
        }
        if (field.byteOffset > definition.maximumFrameLength ||
            width > definition.maximumFrameLength - field.byteOffset) {
            add("field.maximum", "Field exceeds maximumFrameLength: " + field.name);
            continue;
        }
        const auto end = field.byteOffset + width;
        if (field.byteOffset < definition.header.size()) {
            add("field.header_overlap", "Field overlaps frame header: " + field.name);
        }
        if (definition.fixedFrameLength && end > *definition.fixedFrameLength) {
            add("field.frame_bounds", "Field lies outside fixed frame: " + field.name);
        }
        ranges.push_back({field.byteOffset, end, field.name});
    }

    std::sort(ranges.begin(), ranges.end(), [](const Range& left, const Range& right) {
        return left.begin < right.begin;
    });
    for (std::size_t index = 1; index < ranges.size(); ++index) {
        if (ranges[index].begin < ranges[index - 1].end) {
            add("field.overlap", "Fields overlap: " + ranges[index - 1].name +
                                     " and " + ranges[index].name);
        }
    }

    if (definition.lengthField) {
        const auto lengthBegin = definition.lengthField->byteOffset;
        const auto lengthEnd = lengthBegin + fieldTypeByteSize(definition.lengthField->type);
        if (lengthBegin < definition.header.size()) {
            add("frame.length.header_overlap", "Length field overlaps frame header");
        }
        for (const auto& range : ranges) {
            if (range.begin < lengthEnd && lengthBegin < range.end) {
                add("frame.length.field_overlap", "Length field overlaps data field: " + range.name);
            }
        }
    }

    const auto checksumWidth = checksumByteSize(definition.checksum.type);
    if (definition.checksum.type != ChecksumType::None && checksumWidth == 0) {
        add("checksum.type", "Unsupported checksum type");
    }
    if (definition.checksum.byteOffset && definition.fixedFrameLength &&
        *definition.checksum.byteOffset + checksumWidth > *definition.fixedFrameLength) {
        add("checksum.offset", "Checksum lies outside fixed frame");
    }
    if (definition.fixedFrameLength && checksumWidth > 0 &&
        !definition.checksum.byteOffset && checksumWidth > *definition.fixedFrameLength) {
        add("checksum.frame_bounds", "Frame is shorter than checksum width");
    }
    if (definition.checksum.rangeLength &&
        (definition.checksum.rangeStart > definition.maximumFrameLength ||
         *definition.checksum.rangeLength >
             definition.maximumFrameLength - definition.checksum.rangeStart)) {
        add("checksum.range", "Checksum range exceeds maximumFrameLength");
    }
    if (checksumWidth > 0 && definition.checksum.byteOffset) {
        const auto checksumBegin = *definition.checksum.byteOffset;
        if (checksumBegin <= definition.maximumFrameLength &&
            checksumWidth <= definition.maximumFrameLength - checksumBegin) {
            const auto checksumEnd = checksumBegin + checksumWidth;
            for (const auto& range : ranges) {
                if (range.begin < checksumEnd && checksumBegin < range.end) {
                    add("checksum.field_overlap", "Checksum overlaps data field: " + range.name);
                }
            }
        }
    }

    return issues;
}

std::string toString(FieldType type) {
    switch (type) {
    case FieldType::UInt8: return "uint8";
    case FieldType::Int8: return "int8";
    case FieldType::UInt16: return "uint16";
    case FieldType::Int16: return "int16";
    case FieldType::UInt32: return "uint32";
    case FieldType::Int32: return "int32";
    case FieldType::Float32: return "float32";
    case FieldType::Float64: return "float64";
    case FieldType::Boolean: return "bool";
    case FieldType::ByteArray: return "byte_array";
    }
    return "unknown";
}

std::string toString(Endian endian) {
    return endian == Endian::Little ? "little" : "big";
}

std::string toString(ChecksumType type) {
    switch (type) {
    case ChecksumType::None: return "none";
    case ChecksumType::Sum8: return "sum8";
    case ChecksumType::Crc8Atm: return "crc8_atm";
    case ChecksumType::Crc16Modbus: return "crc16_modbus";
    case ChecksumType::Crc16CcittFalse: return "crc16_ccitt_false";
    }
    return "unknown";
}

}  // namespace lab::core

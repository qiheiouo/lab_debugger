#include "lab/core/checksum.hpp"

#include <cstddef>

namespace lab::core {

std::uint8_t checksumSum8(std::span<const std::uint8_t> data) noexcept {
    std::uint8_t result = 0;
    for (const auto byte : data) {
        result = static_cast<std::uint8_t>(result + byte);
    }
    return result;
}

std::uint8_t crc8Atm(std::span<const std::uint8_t> data) noexcept {
    std::uint8_t crc = 0;
    for (const auto byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = static_cast<std::uint8_t>(
                (crc & 0x80U) != 0U ? (crc << 1U) ^ 0x07U : crc << 1U);
        }
    }
    return crc;
}

std::uint16_t crc16Modbus(std::span<const std::uint8_t> data) noexcept {
    std::uint16_t crc = 0xFFFFU;
    for (const auto byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = static_cast<std::uint16_t>(
                (crc & 1U) != 0U ? (crc >> 1U) ^ 0xA001U : crc >> 1U);
        }
    }
    return crc;
}

std::uint16_t crc16CcittFalse(std::span<const std::uint8_t> data) noexcept {
    std::uint16_t crc = 0xFFFFU;
    for (const auto byte : data) {
        crc ^= static_cast<std::uint16_t>(byte) << 8U;
        for (int bit = 0; bit < 8; ++bit) {
            crc = static_cast<std::uint16_t>(
                (crc & 0x8000U) != 0U ? (crc << 1U) ^ 0x1021U : crc << 1U);
        }
    }
    return crc;
}

bool verifyFrameChecksum(
    std::span<const std::uint8_t> frame,
    const ChecksumDefinition& definition,
    std::string* error) noexcept {
    if (definition.type == ChecksumType::None) {
        return true;
    }

    const auto width = checksumByteSize(definition.type);
    const auto checksumOffset = definition.byteOffset.value_or(
        frame.size() >= width ? frame.size() - width : frame.size());
    if (checksumOffset + width > frame.size()) {
        if (error) *error = "Checksum offset lies outside frame";
        return false;
    }
    const auto rangeLength = definition.rangeLength.value_or(
        checksumOffset >= definition.rangeStart ? checksumOffset - definition.rangeStart : 0);
    if (definition.rangeStart > frame.size() ||
        rangeLength > frame.size() - definition.rangeStart) {
        if (error) *error = "Checksum input range lies outside frame";
        return false;
    }

    const auto input = frame.subspan(definition.rangeStart, rangeLength);
    std::uint32_t calculated = 0;
    switch (definition.type) {
    case ChecksumType::Sum8:
        calculated = checksumSum8(input);
        break;
    case ChecksumType::Crc8Atm:
        calculated = crc8Atm(input);
        break;
    case ChecksumType::Crc16Modbus:
        calculated = crc16Modbus(input);
        break;
    case ChecksumType::Crc16CcittFalse:
        calculated = crc16CcittFalse(input);
        break;
    case ChecksumType::None:
        return true;
    }

    std::uint32_t stored = 0;
    if (definition.endian == Endian::Little) {
        for (std::size_t index = 0; index < width; ++index) {
            stored |= static_cast<std::uint32_t>(frame[checksumOffset + index]) << (index * 8U);
        }
    } else {
        for (std::size_t index = 0; index < width; ++index) {
            stored = (stored << 8U) | frame[checksumOffset + index];
        }
    }
    if (stored != calculated) {
        if (error) {
            *error = "Checksum mismatch: expected " + std::to_string(stored) +
                     ", calculated " + std::to_string(calculated);
        }
        return false;
    }
    return true;
}

}  // namespace lab::core


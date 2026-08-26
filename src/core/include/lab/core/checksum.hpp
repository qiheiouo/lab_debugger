#pragma once

#include "lab/core/protocol_definition.hpp"

#include <cstdint>
#include <span>
#include <string>

namespace lab::core {

[[nodiscard]] std::uint8_t checksumSum8(std::span<const std::uint8_t> data) noexcept;
[[nodiscard]] std::uint8_t crc8Atm(std::span<const std::uint8_t> data) noexcept;
[[nodiscard]] std::uint16_t crc16Modbus(std::span<const std::uint8_t> data) noexcept;
[[nodiscard]] std::uint16_t crc16CcittFalse(std::span<const std::uint8_t> data) noexcept;

[[nodiscard]] bool verifyFrameChecksum(
    std::span<const std::uint8_t> frame,
    const ChecksumDefinition& definition,
    std::string* error = nullptr) noexcept;

}  // namespace lab::core


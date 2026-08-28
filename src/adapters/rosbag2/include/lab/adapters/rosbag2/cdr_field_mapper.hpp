#pragma once

#include "lab/core/timestamp.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lab::adapters::rosbag2 {

struct CdrNumericField {
    std::string path;
    std::string unit;
    double value{};
};

struct CdrMappingResult {
    bool supported{};
    bool success{};
    lab::core::Timestamp sourceTimestamp{};
    std::vector<CdrNumericField> fields;
    std::string warning;
};

[[nodiscard]] bool hasStructuredCdrMapping(std::string_view messageType) noexcept;

[[nodiscard]] CdrMappingResult mapStructuredCdrFields(
    std::string_view messageType,
    std::span<const std::uint8_t> serializedCdr);

}  // namespace lab::adapters::rosbag2

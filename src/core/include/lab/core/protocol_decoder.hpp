#pragma once

#include "lab/core/protocol_definition.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace lab::core {

using DecodedValue = std::variant<
    bool,
    std::int64_t,
    std::uint64_t,
    double,
    std::vector<std::uint8_t>>;

struct DecodedField {
    std::string name;
    FieldType type{FieldType::UInt8};
    DecodedValue rawValue{std::uint64_t{0}};
    std::optional<double> numericValue;
    std::string unit;
    std::string enumLabel;
};

struct DecodeResult {
    bool success{};
    std::vector<DecodedField> fields;
    std::string error;
};

[[nodiscard]] DecodeResult decodeProtocolFrame(
    std::span<const std::uint8_t> frame,
    const ProtocolDefinition& definition);

[[nodiscard]] std::string decodedValueToString(const DecodedField& field);

}  // namespace lab::core


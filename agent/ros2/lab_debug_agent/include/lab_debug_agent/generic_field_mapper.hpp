#pragma once

#include "lab_debug_agent/field_mapper.hpp"

#include <rclcpp/serialized_message.hpp>

#include <string>

namespace lab_debug_agent {

[[nodiscard]] lab::core::agent::FieldMappingKind inspectGenericFieldMapping(
    const std::string& type,
    std::string* reason = nullptr);

[[nodiscard]] MappedFields mapGenericSerializedFields(
    const std::string& type,
    const rclcpp::SerializedMessage& serialized);

}  // namespace lab_debug_agent

#pragma once

#include "lab/core/agent_protocol.hpp"

#include <rclcpp/serialized_message.hpp>

#include <string>
#include <vector>

namespace lab_debug_agent {

struct MappedFields {
    lab::core::Timestamp sourceTimestamp{};
    std::vector<lab::core::agent::FieldValue> fields;
    std::string warning;
};

[[nodiscard]] MappedFields mapSerializedFields(
    const std::string& type,
    const rclcpp::SerializedMessage& serialized);

}  // namespace lab_debug_agent

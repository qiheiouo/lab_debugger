#pragma once

#include "lab/core/protocol_definition.hpp"

#include <optional>
#include <string_view>
#include <vector>

namespace lab::core {

struct ProtocolLoadResult {
    std::optional<ProtocolDefinition> definition;
    std::vector<ProtocolIssue> issues;

    [[nodiscard]] bool success() const noexcept {
        return definition.has_value() && issues.empty();
    }
};

[[nodiscard]] ProtocolLoadResult loadProtocolJson(std::string_view json);

}  // namespace lab::core


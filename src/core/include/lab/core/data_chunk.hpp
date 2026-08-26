#pragma once

#include "lab/core/timestamp.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace lab::core {

enum class Direction : std::uint8_t { Rx = 0, Tx = 1 };

struct DataChunk {
    std::string sourceId;
    Timestamp sourceTimestamp{};
    Timestamp receiveTimestamp{};
    std::uint64_t sequence{};
    Direction direction{Direction::Rx};
    std::vector<std::uint8_t> payload;
};

}  // namespace lab::core


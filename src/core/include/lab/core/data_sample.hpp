#pragma once

#include "lab/core/timestamp.hpp"

#include <cstdint>
#include <string>

namespace lab::core {

struct DataSample {
    Timestamp timestamp{};
    std::string sourceId;
    std::string field;
    double value{};
    std::string unit;
    std::uint64_t sequence{};
    // timestamp is the effective analysis timeline. These two values retain
    // the evidence used to derive it and default to timestamp for legacy
    // producers that do not provide separate clock domains.
    Timestamp sourceTimestamp{};
    Timestamp receiveTimestamp{};
};

}  // namespace lab::core

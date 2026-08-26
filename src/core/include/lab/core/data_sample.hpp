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
};

}  // namespace lab::core


#pragma once

#include <chrono>
#include <cstdint>

namespace lab::core {

using Timestamp = std::int64_t;

inline Timestamp nowTimestampNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

inline double secondsBetween(Timestamp start, Timestamp end) noexcept {
    return static_cast<double>(end - start) / 1'000'000'000.0;
}

}  // namespace lab::core


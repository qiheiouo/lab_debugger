#pragma once

#include "lab/core/data_chunk.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace lab::core {

struct RawLogIndexEntry {
    std::uint64_t recordOffset{};
    Timestamp timelineTimestamp{};
};

class RawLogReader {
public:
    RawLogReader() = default;

    RawLogReader(const RawLogReader&) = delete;
    RawLogReader& operator=(const RawLogReader&) = delete;

    bool open(const std::filesystem::path& path);
    void close();

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] std::size_t recordCount() const noexcept;
    [[nodiscard]] Timestamp firstTimestamp() const noexcept;
    [[nodiscard]] Timestamp lastTimestamp() const noexcept;
    [[nodiscard]] Timestamp duration() const noexcept;
    [[nodiscard]] bool hasTruncatedTail() const noexcept;
    [[nodiscard]] std::string error() const;
    [[nodiscard]] const std::vector<RawLogIndexEntry>& index() const noexcept;
    [[nodiscard]] std::size_t lowerBound(Timestamp timelineTimestamp) const noexcept;
    [[nodiscard]] std::optional<DataChunk> read(std::size_t recordIndex);

private:
    mutable std::mutex mutex_;
    std::ifstream input_;
    std::vector<RawLogIndexEntry> index_;
    std::string error_;
    bool truncatedTail_{};
};

}  // namespace lab::core

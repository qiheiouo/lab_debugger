#pragma once

#include "lab/core/data_chunk.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace lab::core {

class RawLogWriter {
public:
    RawLogWriter() = default;
    ~RawLogWriter();

    RawLogWriter(const RawLogWriter&) = delete;
    RawLogWriter& operator=(const RawLogWriter&) = delete;

    bool open(const std::filesystem::path& path);
    bool write(const DataChunk& chunk);
    bool close();

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] std::string error() const;

private:
    std::ofstream output_;
    std::string error_;
};

}  // namespace lab::core

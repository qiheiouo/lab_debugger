#pragma once

#include "lab/core/data_sample.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace lab::core {

class CsvStreamParser {
public:
    explicit CsvStreamParser(
        std::vector<std::string> fieldNames = {"field0", "field1", "field2"});

    void setFieldNames(std::vector<std::string> fieldNames);
    void reset();

    [[nodiscard]] std::vector<DataSample> consume(
        std::span<const std::uint8_t> bytes,
        Timestamp timestamp,
        const std::string& sourceId,
        std::uint64_t sequence);

    [[nodiscard]] std::vector<std::vector<DataSample>> consumeBatches(
        std::span<const std::uint8_t> bytes,
        Timestamp timestamp,
        const std::string& sourceId,
        std::uint64_t sequence);

private:
    [[nodiscard]] std::vector<DataSample> parseLine(
        const std::string& line,
        Timestamp timestamp,
        const std::string& sourceId,
        std::uint64_t sequence) const;

    std::vector<std::string> fieldNames_;
    std::string pending_;
    static constexpr std::size_t kMaximumLineLength = 64 * 1024;
};

}  // namespace lab::core

#pragma once

#include "lab/core/time_series_store.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

namespace lab::core {

struct TimeSeriesCsvExportOptions {
    std::vector<std::string> fields;
    Timestamp start{};
    Timestamp end{};
    bool includeUtf8Bom{true};
};

struct TimeSeriesCsvExportSummary {
    std::size_t selectedFieldCount{};
    std::size_t fieldsWithData{};
    std::uint64_t rowCount{};
};

using CsvChunkWriter = std::function<bool(std::string_view)>;

[[nodiscard]] bool exportTimeSeriesCsv(
    const TimeSeriesStore& store,
    const TimeSeriesCsvExportOptions& options,
    const CsvChunkWriter& writer,
    TimeSeriesCsvExportSummary* summary = nullptr,
    std::string* error = nullptr);

[[nodiscard]] bool exportTimeSeriesCsv(
    const TimeSeriesStore& store,
    const TimeSeriesCsvExportOptions& options,
    std::ostream& output,
    TimeSeriesCsvExportSummary* summary = nullptr,
    std::string* error = nullptr);

}  // namespace lab::core

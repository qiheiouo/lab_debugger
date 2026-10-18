#include "lab/core/time_series_csv_export.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <locale>
#include <ostream>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace lab::core {
namespace {

struct ExportSeries {
    std::string field;
    std::vector<DataPoint> points;
    std::size_t index{};
};

std::string csvField(std::string value) {
    if (!value.empty() &&
        (value.front() == '=' || value.front() == '+' || value.front() == '-' ||
         value.front() == '@' || value.front() == '\t' || value.front() == '\r')) {
        value.insert(value.begin(), '\'');
    }
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (const auto character : value) {
        if (character == '"') {
            escaped.push_back('"');
        }
        escaped.push_back(character);
    }
    escaped.push_back('"');
    return escaped;
}

bool fail(std::string message, std::string* error) {
    if (error) {
        *error = std::move(message);
    }
    return false;
}

}  // namespace

bool exportTimeSeriesCsv(
    const TimeSeriesStore& store,
    const TimeSeriesCsvExportOptions& options,
    const CsvChunkWriter& writer,
    TimeSeriesCsvExportSummary* summary,
    std::string* error) {
    if (summary) {
        *summary = {};
    }
    if (error) {
        error->clear();
    }
    if (!writer) {
        return fail("CSV writer is not configured", error);
    }
    if (options.start > options.end) {
        return fail("CSV export start timestamp is after the end timestamp", error);
    }

    std::vector<std::string> fields;
    fields.reserve(options.fields.size());
    std::unordered_set<std::string> seen;
    for (const auto& field : options.fields) {
        if (!field.empty() && seen.insert(field).second) {
            fields.push_back(field);
        }
    }
    if (fields.empty()) {
        return fail("CSV export requires at least one field", error);
    }
    std::sort(fields.begin(), fields.end());

    auto snapshots = store.snapshotMany(fields, options.start, options.end);
    std::vector<ExportSeries> series;
    series.reserve(snapshots.size());
    for (auto& snapshot : snapshots) {
        series.push_back({std::move(snapshot.field), std::move(snapshot.points), 0});
    }

    constexpr std::size_t flushThreshold = 64 * 1024;
    std::string buffer;
    buffer.reserve(flushThreshold + 512);
    if (options.includeUtf8Bom) buffer.append("\xEF\xBB\xBF");
    buffer.append("timestamp_ns,relative_seconds,field,value\n");
    const auto flush = [&]() {
        if (buffer.empty()) return true;
        if (!writer(buffer)) return false;
        buffer.clear();
        return true;
    };

    std::uint64_t rowCount = 0;
    std::ostringstream line;
    line.imbue(std::locale::classic());
    while (true) {
        ExportSeries* next = nullptr;
        for (auto& candidate : series) {
            if (candidate.index >= candidate.points.size()) {
                continue;
            }
            if (!next) {
                next = &candidate;
                continue;
            }
            const auto& candidatePoint = candidate.points[candidate.index];
            const auto& nextPoint = next->points[next->index];
            if (candidatePoint.timestamp < nextPoint.timestamp ||
                (candidatePoint.timestamp == nextPoint.timestamp &&
                 candidate.field < next->field)) {
                next = &candidate;
            }
        }
        if (!next) {
            break;
        }

        const auto& point = next->points[next->index++];
        line.str(std::string{});
        line.clear();
        line << point.timestamp << ',' << std::fixed << std::setprecision(9)
             << secondsBetween(options.start, point.timestamp) << ','
             << csvField(next->field) << ',' << std::defaultfloat
             << std::setprecision(std::numeric_limits<double>::max_digits10)
             << point.value << '\n';
        buffer.append(line.str());
        if (buffer.size() >= flushThreshold && !flush()) {
            return fail("failed while writing CSV sample rows", error);
        }
        ++rowCount;
    }
    if (!flush()) {
        return fail("failed while finalizing the CSV output", error);
    }

    if (summary) {
        summary->selectedFieldCount = fields.size();
        summary->fieldsWithData = series.size();
        summary->rowCount = rowCount;
    }
    return true;
}

bool exportTimeSeriesCsv(
    const TimeSeriesStore& store,
    const TimeSeriesCsvExportOptions& options,
    std::ostream& output,
    TimeSeriesCsvExportSummary* summary,
    std::string* error) {
    return exportTimeSeriesCsv(
        store, options,
        [&output](std::string_view text) {
            output.write(text.data(), static_cast<std::streamsize>(text.size()));
            return output.good();
        },
        summary, error);
}

}  // namespace lab::core

#pragma once

#include "lab/core/data_sample.hpp"
#include "lab/core/ring_buffer.hpp"

#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace lab::core {

struct DataPoint {
    Timestamp timestamp{};
    double value{};
};

struct SeriesStatistics {
    double current{};
    double minimum{};
    double maximum{};
    double average{};
    std::size_t count{};
};

struct SeriesBounds {
    Timestamp earliest{};
    Timestamp latest{};
};

struct NamedSeriesSnapshot {
    std::string field;
    std::vector<DataPoint> points;
};

class TimeSeriesStore {
public:
    explicit TimeSeriesStore(std::size_t pointsPerField = 120'000);

    void append(const DataSample& sample);
    void clear();
    void clear(const std::string& field);

    [[nodiscard]] std::vector<std::string> fields() const;
    [[nodiscard]] std::vector<DataPoint> snapshot(
        const std::string& field,
        Timestamp since = 0,
        Timestamp until = std::numeric_limits<Timestamp>::max()) const;
    [[nodiscard]] std::vector<NamedSeriesSnapshot> snapshotMany(
        const std::vector<std::string>& fields,
        Timestamp since = 0,
        Timestamp until = std::numeric_limits<Timestamp>::max()) const;
    [[nodiscard]] std::vector<DataPoint> downsampleMinMax(
        const std::string& field,
        Timestamp since,
        std::size_t maximumPoints,
        Timestamp until = std::numeric_limits<Timestamp>::max()) const;
    [[nodiscard]] std::optional<SeriesBounds> bounds(
        const std::string& field) const;
    [[nodiscard]] std::optional<double> interpolate(
        const std::string& field,
        Timestamp timestamp) const;
    [[nodiscard]] std::optional<SeriesStatistics> statistics(
        const std::string& field,
        Timestamp since = 0,
        Timestamp until = std::numeric_limits<Timestamp>::max()) const;

private:
    struct Series {
        explicit Series(std::size_t capacity) : points(capacity) {}
        RingBuffer<DataPoint> points;
        bool ordered{true};
    };

    [[nodiscard]] static std::vector<DataPoint> copyRange(
        const Series& series,
        Timestamp since,
        Timestamp until);

    std::size_t pointsPerField_;
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<Series>> series_;
};

}  // namespace lab::core

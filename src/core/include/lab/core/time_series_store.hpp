#pragma once

#include "lab/core/data_sample.hpp"
#include "lab/core/ring_buffer.hpp"

#include <cstddef>
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

class TimeSeriesStore {
public:
    explicit TimeSeriesStore(std::size_t pointsPerField = 120'000);

    void append(const DataSample& sample);
    void clear();
    void clear(const std::string& field);

    [[nodiscard]] std::vector<std::string> fields() const;
    [[nodiscard]] std::vector<DataPoint> snapshot(
        const std::string& field,
        Timestamp since = 0) const;
    [[nodiscard]] std::vector<DataPoint> downsampleMinMax(
        const std::string& field,
        Timestamp since,
        std::size_t maximumPoints) const;
    [[nodiscard]] std::optional<double> interpolate(
        const std::string& field,
        Timestamp timestamp) const;
    [[nodiscard]] std::optional<SeriesStatistics> statistics(
        const std::string& field,
        Timestamp since = 0) const;

private:
    struct Series {
        explicit Series(std::size_t capacity) : points(capacity) {}
        RingBuffer<DataPoint> points;
    };

    std::size_t pointsPerField_;
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<Series>> series_;
};

}  // namespace lab::core

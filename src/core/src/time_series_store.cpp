#include "lab/core/time_series_store.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace lab::core {

TimeSeriesStore::TimeSeriesStore(std::size_t pointsPerField)
    : pointsPerField_(pointsPerField) {
    if (pointsPerField_ == 0) {
        throw std::invalid_argument("pointsPerField must be positive");
    }
}

void TimeSeriesStore::append(const DataSample& sample) {
    std::unique_lock lock(mutex_);
    auto& series = series_[sample.field];
    if (!series) {
        series = std::make_unique<Series>(pointsPerField_);
    }
    series->points.push(DataPoint{sample.timestamp, sample.value});
}

void TimeSeriesStore::clear() {
    std::unique_lock lock(mutex_);
    series_.clear();
}

void TimeSeriesStore::clear(const std::string& field) {
    std::unique_lock lock(mutex_);
    series_.erase(field);
}

std::vector<std::string> TimeSeriesStore::fields() const {
    std::shared_lock lock(mutex_);
    std::vector<std::string> names;
    names.reserve(series_.size());
    for (const auto& [name, ignored] : series_) {
        static_cast<void>(ignored);
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<DataPoint> TimeSeriesStore::snapshot(
    const std::string& field,
    Timestamp since) const {
    std::shared_lock lock(mutex_);
    const auto iterator = series_.find(field);
    if (iterator == series_.end()) {
        return {};
    }
    auto points = iterator->second->points.snapshot();
    if (since == 0) {
        return points;
    }
    const auto first = std::lower_bound(
        points.begin(), points.end(), since,
        [](const DataPoint& point, Timestamp timestamp) {
            return point.timestamp < timestamp;
        });
    points.erase(points.begin(), first);
    return points;
}

std::vector<DataPoint> TimeSeriesStore::downsampleMinMax(
    const std::string& field,
    Timestamp since,
    std::size_t maximumPoints) const {
    const auto points = snapshot(field, since);
    if (maximumPoints == 0 || points.empty()) {
        return {};
    }
    if (points.size() <= maximumPoints) {
        return points;
    }
    if (maximumPoints == 1) {
        return {points.back()};
    }

    const auto bucketCount = std::max<std::size_t>(1, maximumPoints / 2);
    const auto bucketSize = (points.size() + bucketCount - 1) / bucketCount;
    std::vector<DataPoint> result;
    result.reserve(std::min(points.size(), bucketCount * 2));
    for (std::size_t start = 0; start < points.size(); start += bucketSize) {
        const auto end = std::min(points.size(), start + bucketSize);
        auto minimum = start;
        auto maximum = start;
        for (auto index = start + 1; index < end; ++index) {
            if (points[index].value < points[minimum].value) {
                minimum = index;
            }
            if (points[index].value > points[maximum].value) {
                maximum = index;
            }
        }
        if (minimum == maximum) {
            result.push_back(points[minimum]);
        } else if (minimum < maximum) {
            result.push_back(points[minimum]);
            result.push_back(points[maximum]);
        } else {
            result.push_back(points[maximum]);
            result.push_back(points[minimum]);
        }
    }
    return result;
}

std::optional<double> TimeSeriesStore::interpolate(
    const std::string& field,
    Timestamp timestamp) const {
    const auto points = snapshot(field);
    if (points.empty() || timestamp < points.front().timestamp ||
        timestamp > points.back().timestamp) {
        return std::nullopt;
    }
    const auto right = std::lower_bound(
        points.begin(), points.end(), timestamp,
        [](const DataPoint& point, Timestamp target) {
            return point.timestamp < target;
        });
    if (right == points.end()) {
        return points.back().value;
    }
    if (right->timestamp == timestamp || right == points.begin()) {
        return right->value;
    }
    const auto left = std::prev(right);
    const auto span = static_cast<double>(right->timestamp - left->timestamp);
    if (span <= 0.0) {
        return right->value;
    }
    const auto ratio = static_cast<double>(timestamp - left->timestamp) / span;
    return left->value + (right->value - left->value) * ratio;
}

std::optional<SeriesStatistics> TimeSeriesStore::statistics(
    const std::string& field,
    Timestamp since) const {
    const auto points = snapshot(field, since);
    if (points.empty()) {
        return std::nullopt;
    }

    SeriesStatistics result;
    result.current = points.back().value;
    result.minimum = std::numeric_limits<double>::max();
    result.maximum = std::numeric_limits<double>::lowest();
    long double sum = 0.0;
    for (const auto& point : points) {
        result.minimum = std::min(result.minimum, point.value);
        result.maximum = std::max(result.maximum, point.value);
        sum += point.value;
    }
    result.count = points.size();
    result.average = static_cast<double>(sum / points.size());
    return result;
}

}  // namespace lab::core

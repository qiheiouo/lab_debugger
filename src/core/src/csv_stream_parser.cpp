#include "lab/core/csv_stream_parser.hpp"

#include <charconv>
#include <cctype>
#include <string_view>

namespace lab::core {
namespace {

std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

}  // namespace

CsvStreamParser::CsvStreamParser(std::vector<std::string> fieldNames) {
    setFieldNames(std::move(fieldNames));
}

void CsvStreamParser::setFieldNames(std::vector<std::string> fieldNames) {
    fieldNames_.clear();
    fieldNames_.reserve(fieldNames.size());
    for (std::size_t index = 0; index < fieldNames.size(); ++index) {
        auto name = std::string(trim(fieldNames[index]));
        fieldNames_.push_back(name.empty() ? "field" + std::to_string(index)
                                           : std::move(name));
    }
    if (fieldNames_.empty()) {
        fieldNames_.push_back("field0");
    }
}

void CsvStreamParser::reset() {
    pending_.clear();
}

std::vector<DataSample> CsvStreamParser::consume(
    std::span<const std::uint8_t> bytes,
    Timestamp timestamp,
    const std::string& sourceId,
    std::uint64_t sequence) {
    auto batches = consumeBatches(bytes, timestamp, sourceId, sequence);
    std::vector<DataSample> samples;
    for (auto& batch : batches) {
        samples.insert(samples.end(),
                       std::make_move_iterator(batch.begin()),
                       std::make_move_iterator(batch.end()));
    }
    return samples;
}

std::vector<std::vector<DataSample>> CsvStreamParser::consumeBatches(
    std::span<const std::uint8_t> bytes,
    Timestamp timestamp,
    const std::string& sourceId,
    std::uint64_t sequence) {
    pending_.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (pending_.size() > kMaximumLineLength && pending_.find('\n') == std::string::npos) {
        pending_.clear();
        return {};
    }

    std::vector<std::vector<DataSample>> batches;
    std::size_t position = 0;
    while (true) {
        const auto newline = pending_.find('\n', position);
        if (newline == std::string::npos) {
            pending_.erase(0, position);
            break;
        }
        auto line = pending_.substr(position, newline - position);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        auto parsed = parseLine(line, timestamp, sourceId, sequence);
        if (!parsed.empty()) batches.push_back(std::move(parsed));
        position = newline + 1;
    }
    return batches;
}

std::vector<DataSample> CsvStreamParser::parseLine(
    const std::string& line,
    Timestamp timestamp,
    const std::string& sourceId,
    std::uint64_t sequence) const {
    std::vector<DataSample> result;
    std::size_t start = 0;
    std::size_t fieldIndex = 0;
    while (start <= line.size() && fieldIndex < fieldNames_.size()) {
        const auto comma = line.find(',', start);
        const auto end = comma == std::string::npos ? line.size() : comma;
        const auto token = trim(std::string_view(line).substr(start, end - start));
        double value{};
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
        if (token.empty() || parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) {
            return {};
        }
        result.push_back(DataSample{
            timestamp, sourceId, fieldNames_[fieldIndex], value, {}, sequence});
        ++fieldIndex;
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return result;
}

}  // namespace lab::core

#include "lab/core/raw_log_reader.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <type_traits>

namespace lab::core {
namespace {

constexpr std::array<char, 8> fileMagic{'L', 'D', 'B', 'G', 'R', 'A', 'W', '1'};
constexpr std::uint32_t recordMagic = 0x4C444252;
constexpr std::uint32_t maximumSourceIdBytes = 1024 * 1024;
constexpr std::uint32_t maximumPayloadBytes = 64 * 1024 * 1024;

template <typename T>
bool readLittle(std::istream& stream, T& value) {
    static_assert(std::is_integral_v<T>);
    std::array<std::uint8_t, sizeof(T)> bytes{};
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
        return false;
    }
    using Unsigned = std::make_unsigned_t<T>;
    Unsigned result{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        result |= static_cast<Unsigned>(bytes[index]) << (index * 8U);
    }
    std::memcpy(&value, &result, sizeof(value));
    return true;
}

}  // namespace

bool RawLogReader::open(const std::filesystem::path& path) {
    std::scoped_lock lock(mutex_);
    input_.close();
    input_.clear();
    index_.clear();
    error_.clear();
    truncatedTail_ = false;

    input_.open(path, std::ios::binary);
    if (!input_) {
        error_ = "cannot open raw log";
        return false;
    }
    std::error_code sizeError;
    const auto fileSize = std::filesystem::file_size(path, sizeError);
    if (sizeError) {
        error_ = "cannot determine raw log size";
        input_.close();
        return false;
    }

    std::array<char, fileMagic.size()> magic{};
    if (!input_.read(magic.data(), magic.size()) || magic != fileMagic) {
        error_ = "invalid raw log header";
        input_.close();
        return false;
    }

    Timestamp lastTimeline{};
    while (true) {
        const auto recordOffset = input_.tellg();
        if (recordOffset < 0) {
            error_ = "cannot determine raw log offset";
            input_.close();
            index_.clear();
            return false;
        }

        std::uint32_t magicValue{};
        if (!readLittle(input_, magicValue)) {
            if (input_.eof()) {
                input_.clear();
                input_.seekg(0, std::ios::end);
                const auto end = input_.tellg();
                truncatedTail_ = end > recordOffset;
                break;
            }
            error_ = "cannot read raw record header";
            input_.close();
            index_.clear();
            return false;
        }
        if (magicValue != recordMagic) {
            error_ = "invalid raw record marker at offset " +
                     std::to_string(static_cast<std::uint64_t>(recordOffset));
            input_.close();
            index_.clear();
            return false;
        }

        RawLogIndexEntry entry;
        entry.recordOffset = static_cast<std::uint64_t>(recordOffset);
        Timestamp sourceTimestamp{};
        Timestamp receiveTimestamp{};
        std::uint64_t sequence{};
        std::uint8_t direction{};
        std::uint32_t sourceIdBytes{};
        std::uint32_t payloadBytes{};
        if (!readLittle(input_, sourceTimestamp) ||
            !readLittle(input_, receiveTimestamp) ||
            !readLittle(input_, sequence) ||
            !readLittle(input_, direction) ||
            !readLittle(input_, sourceIdBytes) ||
            !readLittle(input_, payloadBytes)) {
            truncatedTail_ = true;
            break;
        }
        if (direction > static_cast<std::uint8_t>(Direction::Tx)) {
            error_ = "invalid direction in raw record";
            input_.close();
            index_.clear();
            return false;
        }
        if (sourceIdBytes > maximumSourceIdBytes || payloadBytes > maximumPayloadBytes) {
            error_ = "raw record length exceeds safety limit";
            input_.close();
            index_.clear();
            return false;
        }
        const auto dataOffset = static_cast<std::uint64_t>(input_.tellg());
        entry.timelineTimestamp = index_.empty()
                                      ? receiveTimestamp
                                      : std::max(lastTimeline, receiveTimestamp);
        lastTimeline = entry.timelineTimestamp;

        const auto skip = static_cast<std::uint64_t>(sourceIdBytes) + payloadBytes;
        if (dataOffset > fileSize || skip > fileSize - dataOffset) {
            truncatedTail_ = true;
            break;
        }
        input_.seekg(static_cast<std::streamoff>(skip), std::ios::cur);
        if (!input_) {
            truncatedTail_ = true;
            input_.clear();
            break;
        }
        index_.push_back(entry);
    }

    input_.clear();
    return true;
}

void RawLogReader::close() {
    std::scoped_lock lock(mutex_);
    input_.close();
    index_.clear();
    error_.clear();
    truncatedTail_ = false;
}

bool RawLogReader::isOpen() const noexcept {
    std::scoped_lock lock(mutex_);
    return input_.is_open();
}

std::size_t RawLogReader::recordCount() const noexcept {
    std::scoped_lock lock(mutex_);
    return index_.size();
}

Timestamp RawLogReader::firstTimestamp() const noexcept {
    std::scoped_lock lock(mutex_);
    return index_.empty() ? 0 : index_.front().timelineTimestamp;
}

Timestamp RawLogReader::lastTimestamp() const noexcept {
    std::scoped_lock lock(mutex_);
    return index_.empty() ? 0 : index_.back().timelineTimestamp;
}

Timestamp RawLogReader::duration() const noexcept {
    std::scoped_lock lock(mutex_);
    return index_.empty() ? 0
                          : index_.back().timelineTimestamp - index_.front().timelineTimestamp;
}

bool RawLogReader::hasTruncatedTail() const noexcept {
    std::scoped_lock lock(mutex_);
    return truncatedTail_;
}

std::string RawLogReader::error() const {
    std::scoped_lock lock(mutex_);
    return error_;
}

const std::vector<RawLogIndexEntry>& RawLogReader::index() const noexcept {
    return index_;
}

std::size_t RawLogReader::lowerBound(Timestamp timelineTimestamp) const noexcept {
    std::scoped_lock lock(mutex_);
    return static_cast<std::size_t>(std::lower_bound(
        index_.begin(), index_.end(), timelineTimestamp,
        [](const RawLogIndexEntry& entry, Timestamp timestamp) {
            return entry.timelineTimestamp < timestamp;
        }) - index_.begin());
}

std::optional<DataChunk> RawLogReader::read(std::size_t recordIndex) {
    std::scoped_lock lock(mutex_);
    if (!input_.is_open() || recordIndex >= index_.size()) {
        return std::nullopt;
    }
    const auto& entry = index_[recordIndex];
    input_.clear();
    input_.seekg(static_cast<std::streamoff>(entry.recordOffset));
    if (!input_) {
        error_ = "cannot seek to raw record";
        return std::nullopt;
    }

    std::uint32_t magicValue{};
    std::uint8_t direction{};
    std::uint32_t sourceIdBytes{};
    std::uint32_t payloadBytes{};
    DataChunk chunk;
    if (!readLittle(input_, magicValue) || magicValue != recordMagic ||
        !readLittle(input_, chunk.sourceTimestamp) ||
        !readLittle(input_, chunk.receiveTimestamp) ||
        !readLittle(input_, chunk.sequence) ||
        !readLittle(input_, direction) ||
        !readLittle(input_, sourceIdBytes) ||
        !readLittle(input_, payloadBytes) ||
        direction > static_cast<std::uint8_t>(Direction::Tx) ||
        sourceIdBytes > maximumSourceIdBytes || payloadBytes > maximumPayloadBytes) {
        error_ = "raw record header became unreadable";
        return std::nullopt;
    }
    chunk.direction = static_cast<Direction>(direction);
    chunk.sourceId.resize(sourceIdBytes);
    chunk.payload.resize(payloadBytes);
    if (!input_.read(chunk.sourceId.data(), static_cast<std::streamsize>(sourceIdBytes)) ||
        !input_.read(reinterpret_cast<char*>(chunk.payload.data()),
                     static_cast<std::streamsize>(payloadBytes))) {
        error_ = "raw record became unreadable";
        return std::nullopt;
    }
    return chunk;
}

}  // namespace lab::core

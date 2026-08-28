#include "lab/core/raw_log_writer.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

namespace lab::core {
namespace {

constexpr std::array<char, 8> fileMagic{'L', 'D', 'B', 'G', 'R', 'A', 'W', '1'};
constexpr std::uint32_t recordMagic = 0x4C444252;
constexpr std::size_t maximumSourceIdBytes = 1024 * 1024;
constexpr std::size_t maximumPayloadBytes = 64 * 1024 * 1024;

template <typename T>
void writeLittle(std::ofstream& stream, T value) {
    static_assert(std::is_integral_v<T>);
    using Unsigned = std::make_unsigned_t<T>;
    Unsigned bits{};
    std::memcpy(&bits, &value, sizeof(value));
    std::array<std::uint8_t, sizeof(T)> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(bits >> (index * 8U));
    }
    stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

}  // namespace

RawLogWriter::~RawLogWriter() {
    close();
}

bool RawLogWriter::open(const std::filesystem::path& path) {
    close();
    error_.clear();
    output_.open(path, std::ios::binary | std::ios::trunc);
    if (!output_) {
        error_ = "cannot open raw log";
        return false;
    }
    output_.write(fileMagic.data(), fileMagic.size());
    if (!output_) {
        error_ = "cannot write raw log header";
        output_.close();
        return false;
    }
    return true;
}

bool RawLogWriter::write(const DataChunk& chunk) {
    if (!output_.is_open()) {
        error_ = "raw log is not open";
        return false;
    }
    if (chunk.sourceId.size() > maximumSourceIdBytes) {
        error_ = "raw source id exceeds safety limit";
        return false;
    }
    if (chunk.payload.size() > maximumPayloadBytes) {
        error_ = "raw payload exceeds safety limit";
        return false;
    }
    if (chunk.sourceId.size() > std::numeric_limits<std::uint32_t>::max() ||
        chunk.payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        error_ = "raw record is too large";
        return false;
    }

    writeLittle(output_, recordMagic);
    writeLittle(output_, chunk.sourceTimestamp);
    writeLittle(output_, chunk.receiveTimestamp);
    writeLittle(output_, chunk.sequence);
    writeLittle(output_, static_cast<std::uint8_t>(chunk.direction));
    writeLittle(output_, static_cast<std::uint32_t>(chunk.sourceId.size()));
    writeLittle(output_, static_cast<std::uint32_t>(chunk.payload.size()));
    output_.write(chunk.sourceId.data(), static_cast<std::streamsize>(chunk.sourceId.size()));
    output_.write(reinterpret_cast<const char*>(chunk.payload.data()),
                  static_cast<std::streamsize>(chunk.payload.size()));
    if (!output_) {
        error_ = "cannot write raw record";
        return false;
    }
    return true;
}

bool RawLogWriter::close() {
    if (!output_.is_open()) {
        return error_.empty();
    }
    output_.flush();
    const auto succeeded = static_cast<bool>(output_);
    output_.close();
    if (!succeeded && error_.empty()) {
        error_ = "cannot flush raw log";
    }
    return succeeded;
}

bool RawLogWriter::isOpen() const noexcept {
    return output_.is_open();
}

std::string RawLogWriter::error() const {
    return error_;
}

}  // namespace lab::core

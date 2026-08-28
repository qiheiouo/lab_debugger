#include "lab/adapters/rosbag2/cdr_field_mapper.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace lab::adapters::rosbag2 {
namespace {

constexpr std::size_t maximumSequenceItems = 1024;
constexpr std::size_t maximumStringBytes = 1024 * 1024;

class CdrReader {
public:
    explicit CdrReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {
        if (bytes_.size() < 4) {
            fail("serialized CDR is shorter than its 4-byte encapsulation header");
            return;
        }
        const auto representation = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes_[0]) << 8U |
            static_cast<std::uint16_t>(bytes_[1]));
        if (representation == 0x0000U) {
            littleEndian_ = false;
        } else if (representation == 0x0001U) {
            littleEndian_ = true;
        } else {
            fail("unsupported CDR encapsulation representation");
            return;
        }
        position_ = 4;
        alignmentBase_ = position_;
    }

    [[nodiscard]] bool good() const noexcept { return error_.empty(); }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

    std::optional<bool> boolean() {
        const auto value = unsignedInteger(1);
        if (!value) return std::nullopt;
        if (*value > 1) {
            fail("CDR boolean is not 0 or 1");
            return std::nullopt;
        }
        return *value != 0;
    }

    std::optional<std::int32_t> int32() {
        const auto value = unsignedInteger(4);
        if (!value) return std::nullopt;
        return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(*value));
    }

    std::optional<std::uint32_t> uint32() {
        const auto value = unsignedInteger(4);
        if (!value) return std::nullopt;
        return static_cast<std::uint32_t>(*value);
    }

    std::optional<float> float32() {
        const auto value = uint32();
        if (!value) return std::nullopt;
        return std::bit_cast<float>(*value);
    }

    std::optional<double> float64() {
        const auto value = unsignedInteger(8);
        if (!value) return std::nullopt;
        return std::bit_cast<double>(*value);
    }

    std::optional<std::string> string() {
        const auto size = uint32();
        if (!size) return std::nullopt;
        if (*size == 0 || *size > maximumStringBytes) {
            fail("CDR string length exceeds the supported range");
            return std::nullopt;
        }
        if (!require(*size)) return std::nullopt;
        if (bytes_[position_ + *size - 1] != 0) {
            fail("CDR string is missing its null terminator");
            return std::nullopt;
        }
        std::string result(
            reinterpret_cast<const char*>(bytes_.data() + position_), *size - 1);
        position_ += *size;
        return result;
    }

    std::optional<std::vector<std::string>> stringSequence() {
        const auto count = sequenceLength();
        if (!count) return std::nullopt;
        std::vector<std::string> result;
        result.reserve(*count);
        for (std::size_t index = 0; index < *count; ++index) {
            auto value = string();
            if (!value) return std::nullopt;
            result.push_back(std::move(*value));
        }
        return result;
    }

    std::optional<std::vector<double>> float64Sequence() {
        const auto count = sequenceLength();
        if (!count) return std::nullopt;
        std::vector<double> result;
        result.reserve(*count);
        for (std::size_t index = 0; index < *count; ++index) {
            const auto value = float64();
            if (!value) return std::nullopt;
            result.push_back(*value);
        }
        return result;
    }

private:
    std::optional<std::size_t> sequenceLength() {
        const auto count = uint32();
        if (!count) return std::nullopt;
        if (*count > maximumSequenceItems) {
            fail("CDR sequence exceeds the 1024-item safety limit");
            return std::nullopt;
        }
        return static_cast<std::size_t>(*count);
    }

    std::optional<std::uint64_t> unsignedInteger(std::size_t width) {
        if (!align(width) || !require(width)) return std::nullopt;
        std::uint64_t value{};
        if (littleEndian_) {
            for (std::size_t index = 0; index < width; ++index) {
                value |= static_cast<std::uint64_t>(bytes_[position_ + index])
                         << (index * 8U);
            }
        } else {
            for (std::size_t index = 0; index < width; ++index) {
                value = (value << 8U) | bytes_[position_ + index];
            }
        }
        position_ += width;
        return value;
    }

    bool align(std::size_t alignment) {
        if (!good()) return false;
        const auto relative = position_ - alignmentBase_;
        const auto padding = (alignment - relative % alignment) % alignment;
        if (!require(padding)) return false;
        position_ += padding;
        return true;
    }

    bool require(std::size_t count) {
        if (!good()) return false;
        if (count > bytes_.size() - position_) {
            fail("serialized CDR is truncated");
            return false;
        }
        return true;
    }

    void fail(std::string message) {
        if (error_.empty()) error_ = std::move(message);
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t position_{};
    std::size_t alignmentBase_{};
    bool littleEndian_{};
    std::string error_;
};

void addField(std::vector<CdrNumericField>& fields,
              std::string path,
              std::string unit,
              double value) {
    if (!std::isfinite(value)) {
        throw std::runtime_error("CDR numeric field is not finite");
    }
    fields.push_back({std::move(path), std::move(unit), value});
}

bool readHeader(CdrReader& reader, lab::core::Timestamp& timestamp) {
    const auto seconds = reader.int32();
    const auto nanoseconds = reader.uint32();
    const auto frame = reader.string();
    if (!seconds || !nanoseconds || !frame || *nanoseconds >= 1'000'000'000U) {
        return false;
    }
    timestamp = static_cast<lab::core::Timestamp>(*seconds) * 1'000'000'000LL +
                static_cast<lab::core::Timestamp>(*nanoseconds);
    return true;
}

bool readVector3(CdrReader& reader,
                 std::vector<CdrNumericField>& fields,
                 const std::string& prefix,
                 const std::string& unit) {
    const auto x = reader.float64();
    const auto y = reader.float64();
    const auto z = reader.float64();
    if (!x || !y || !z) return false;
    addField(fields, prefix + ".x", unit, *x);
    addField(fields, prefix + ".y", unit, *y);
    addField(fields, prefix + ".z", unit, *z);
    return true;
}

bool readQuaternion(CdrReader& reader,
                    std::vector<CdrNumericField>& fields,
                    const std::string& prefix) {
    const auto x = reader.float64();
    const auto y = reader.float64();
    const auto z = reader.float64();
    const auto w = reader.float64();
    if (!x || !y || !z || !w) return false;
    addField(fields, prefix + ".x", {}, *x);
    addField(fields, prefix + ".y", {}, *y);
    addField(fields, prefix + ".z", {}, *z);
    addField(fields, prefix + ".w", {}, *w);
    return true;
}

bool readPose(CdrReader& reader,
              std::vector<CdrNumericField>& fields,
              const std::string& prefix) {
    return readVector3(reader, fields, prefix + ".position", "m") &&
           readQuaternion(reader, fields, prefix + ".orientation");
}

bool readTwist(CdrReader& reader,
               std::vector<CdrNumericField>& fields,
               const std::string& prefix) {
    const auto path = [&prefix](std::string_view name) {
        return prefix.empty() ? std::string(name)
                              : prefix + "." + std::string(name);
    };
    return readVector3(reader, fields, path("linear"), "m/s") &&
           readVector3(reader, fields, path("angular"), "rad/s");
}

bool skipFloat64(CdrReader& reader, std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) {
        if (!reader.float64()) return false;
    }
    return true;
}

bool mapKnownType(std::string_view type,
                  CdrReader& reader,
                  CdrMappingResult& result) {
    if (type == "std_msgs/msg/Float64") {
        const auto value = reader.float64();
        if (!value) return false;
        addField(result.fields, "data", {}, *value);
    } else if (type == "std_msgs/msg/Float32") {
        const auto value = reader.float32();
        if (!value) return false;
        addField(result.fields, "data", {}, static_cast<double>(*value));
    } else if (type == "std_msgs/msg/Int32") {
        const auto value = reader.int32();
        if (!value) return false;
        addField(result.fields, "data", {}, static_cast<double>(*value));
    } else if (type == "std_msgs/msg/UInt32") {
        const auto value = reader.uint32();
        if (!value) return false;
        addField(result.fields, "data", {}, static_cast<double>(*value));
    } else if (type == "std_msgs/msg/Bool") {
        const auto value = reader.boolean();
        if (!value) return false;
        addField(result.fields, "data", {}, *value ? 1.0 : 0.0);
    } else if (type == "geometry_msgs/msg/Twist") {
        return readTwist(reader, result.fields, {});
    } else if (type == "geometry_msgs/msg/TwistStamped") {
        return readHeader(reader, result.sourceTimestamp) &&
               readTwist(reader, result.fields, "twist");
    } else if (type == "geometry_msgs/msg/PoseStamped") {
        return readHeader(reader, result.sourceTimestamp) &&
               readPose(reader, result.fields, "pose");
    } else if (type == "sensor_msgs/msg/Imu") {
        return readHeader(reader, result.sourceTimestamp) &&
               readQuaternion(reader, result.fields, "orientation") &&
               skipFloat64(reader, 9) &&
               readVector3(reader, result.fields, "angular_velocity", "rad/s") &&
               skipFloat64(reader, 9) &&
               readVector3(reader, result.fields, "linear_acceleration", "m/s^2") &&
               skipFloat64(reader, 9);
    } else if (type == "sensor_msgs/msg/JointState") {
        if (!readHeader(reader, result.sourceTimestamp)) return false;
        const auto names = reader.stringSequence();
        const auto positions = reader.float64Sequence();
        const auto velocities = reader.float64Sequence();
        const auto efforts = reader.float64Sequence();
        if (!names || !positions || !velocities || !efforts) return false;
        for (std::size_t index = 0; index < std::min(names->size(), positions->size()); ++index) {
            addField(result.fields, "position." + (*names)[index], "rad", (*positions)[index]);
        }
        for (std::size_t index = 0; index < std::min(names->size(), velocities->size()); ++index) {
            addField(result.fields, "velocity." + (*names)[index], "rad/s", (*velocities)[index]);
        }
        for (std::size_t index = 0; index < std::min(names->size(), efforts->size()); ++index) {
            addField(result.fields, "effort." + (*names)[index], {}, (*efforts)[index]);
        }
    } else if (type == "nav_msgs/msg/Odometry") {
        if (!readHeader(reader, result.sourceTimestamp)) return false;
        if (!reader.string()) return false;
        return readPose(reader, result.fields, "pose.pose") &&
               skipFloat64(reader, 36) &&
               readTwist(reader, result.fields, "twist.twist") &&
               skipFloat64(reader, 36);
    } else {
        return false;
    }
    return true;
}

}  // namespace

bool hasStructuredCdrMapping(std::string_view messageType) noexcept {
    return messageType == "std_msgs/msg/Float64" ||
           messageType == "std_msgs/msg/Float32" ||
           messageType == "std_msgs/msg/Int32" ||
           messageType == "std_msgs/msg/UInt32" ||
           messageType == "std_msgs/msg/Bool" ||
           messageType == "geometry_msgs/msg/Twist" ||
           messageType == "geometry_msgs/msg/TwistStamped" ||
           messageType == "geometry_msgs/msg/PoseStamped" ||
           messageType == "sensor_msgs/msg/Imu" ||
           messageType == "sensor_msgs/msg/JointState" ||
           messageType == "nav_msgs/msg/Odometry";
}

CdrMappingResult mapStructuredCdrFields(
    std::string_view messageType,
    std::span<const std::uint8_t> serializedCdr) {
    CdrMappingResult result;
    result.supported = hasStructuredCdrMapping(messageType);
    if (!result.supported) return result;

    try {
        CdrReader reader(serializedCdr);
        if (!reader.good() || !mapKnownType(messageType, reader, result) || !reader.good()) {
            result.fields.clear();
            result.sourceTimestamp = 0;
            result.warning = "Structured CDR mapping failed for " +
                             std::string(messageType) + ": " +
                             (reader.error().empty() ? "invalid message layout" : reader.error());
            return result;
        }
        result.success = true;
    } catch (const std::exception& exception) {
        result.fields.clear();
        result.sourceTimestamp = 0;
        result.warning = "Structured CDR mapping failed for " +
                         std::string(messageType) + ": " + exception.what();
    }
    return result;
}

}  // namespace lab::adapters::rosbag2

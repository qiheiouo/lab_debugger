#include "lab/adapters/rosbag2/cdr_field_mapper.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class CdrWriter {
public:
    explicit CdrWriter(bool littleEndian = true) : littleEndian_(littleEndian) {
        bytes_ = {0, static_cast<std::uint8_t>(littleEndian ? 1 : 0), 0, 0};
    }

    void boolean(bool value) { integer(value ? 1U : 0U, 1); }
    void int32(std::int32_t value) {
        integer(std::bit_cast<std::uint32_t>(value), 4);
    }
    void uint32(std::uint32_t value) { integer(value, 4); }
    void float32(float value) { integer(std::bit_cast<std::uint32_t>(value), 4); }
    void float64(double value) { integer(std::bit_cast<std::uint64_t>(value), 8); }

    void string(std::string_view value) {
        uint32(static_cast<std::uint32_t>(value.size() + 1));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
        bytes_.push_back(0);
    }

    void header(std::int32_t seconds,
                std::uint32_t nanoseconds,
                std::string_view frame) {
        int32(seconds);
        uint32(nanoseconds);
        string(frame);
    }

    void strings(const std::vector<std::string>& values) {
        uint32(static_cast<std::uint32_t>(values.size()));
        for (const auto& value : values) string(value);
    }

    void doubles(const std::vector<double>& values) {
        uint32(static_cast<std::uint32_t>(values.size()));
        for (const auto value : values) float64(value);
    }

    void fixedDoubles(std::size_t count, double first = 0.0) {
        for (std::size_t index = 0; index < count; ++index) {
            float64(first + static_cast<double>(index));
        }
    }

    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept {
        return bytes_;
    }

private:
    void integer(std::uint64_t value, std::size_t width) {
        const auto relative = bytes_.size() - 4;
        const auto padding = (width - relative % width) % width;
        bytes_.insert(bytes_.end(), padding, 0);
        if (littleEndian_) {
            for (std::size_t index = 0; index < width; ++index) {
                bytes_.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
            }
        } else {
            for (std::size_t index = 0; index < width; ++index) {
                const auto shift = (width - index - 1) * 8U;
                bytes_.push_back(static_cast<std::uint8_t>(value >> shift));
            }
        }
    }

    bool littleEndian_;
    std::vector<std::uint8_t> bytes_;
};

double field(const lab::adapters::rosbag2::CdrMappingResult& result,
             std::string_view path,
             std::string_view unit = {}) {
    const auto item = std::find_if(
        result.fields.begin(), result.fields.end(), [&](const auto& value) {
            return value.path == path;
        });
    require(item != result.fields.end(), "mapped field is present: " + std::string(path));
    require(item->unit == unit, "mapped field unit is exact: " + std::string(path));
    return item->value;
}

void testScalarAndEndian() {
    CdrWriter little;
    little.float64(42.125);
    const auto float64 = lab::adapters::rosbag2::mapStructuredCdrFields(
        "std_msgs/msg/Float64", little.bytes());
    require(float64.supported && float64.success && float64.fields.size() == 1 &&
                field(float64, "data") == 42.125,
            "little-endian Float64 maps exactly");

    CdrWriter big(false);
    big.float32(-2.5F);
    const auto float32 = lab::adapters::rosbag2::mapStructuredCdrFields(
        "std_msgs/msg/Float32", big.bytes());
    require(float32.success && field(float32, "data") == -2.5,
            "big-endian Float32 maps exactly");

    CdrWriter boolean;
    boolean.boolean(true);
    const auto mappedBool = lab::adapters::rosbag2::mapStructuredCdrFields(
        "std_msgs/msg/Bool", boolean.bytes());
    require(mappedBool.success && field(mappedBool, "data") == 1.0,
            "Bool maps to a numeric curve value");
}

void testStampedAndSensorTypes() {
    CdrWriter stamped;
    stamped.header(12, 345, "base_link");
    for (const auto value : {1.0, 2.0, 3.0, 4.0, 5.0, 6.0}) {
        stamped.float64(value);
    }
    const auto twist = lab::adapters::rosbag2::mapStructuredCdrFields(
        "geometry_msgs/msg/TwistStamped", stamped.bytes());
    require(twist.success && twist.sourceTimestamp == 12'000'000'345LL &&
                twist.fields.size() == 6 &&
                field(twist, "twist.linear.y", "m/s") == 2.0 &&
                field(twist, "twist.angular.z", "rad/s") == 6.0,
            "TwistStamped keeps Header time, paths, and units");

    CdrWriter imu;
    imu.header(9, 0, "imu");
    for (const auto value : {0.1, 0.2, 0.3, 0.9}) imu.float64(value);
    imu.fixedDoubles(9);
    for (const auto value : {1.0, 2.0, -1.5}) imu.float64(value);
    imu.fixedDoubles(9);
    for (const auto value : {9.81, 0.0, -9.81}) imu.float64(value);
    imu.fixedDoubles(9);
    const auto mappedImu = lab::adapters::rosbag2::mapStructuredCdrFields(
        "sensor_msgs/msg/Imu", imu.bytes());
    require(mappedImu.success && mappedImu.fields.size() == 10 &&
                mappedImu.sourceTimestamp == 9'000'000'000LL &&
                field(mappedImu, "orientation.w") == 0.9 &&
                field(mappedImu, "angular_velocity.z", "rad/s") == -1.5 &&
                field(mappedImu, "linear_acceleration.x", "m/s^2") == 9.81,
            "Imu skips covariance arrays and preserves semantic fields");
}

void testVariableAndNestedTypes() {
    CdrWriter joints;
    joints.header(3, 4, "arm");
    joints.strings({"left", "right"});
    joints.doubles({0.25, -0.5});
    joints.doubles({1.5});
    joints.doubles({4.0, 5.0});
    const auto mappedJoints = lab::adapters::rosbag2::mapStructuredCdrFields(
        "sensor_msgs/msg/JointState", joints.bytes());
    require(mappedJoints.success && mappedJoints.fields.size() == 5 &&
                field(mappedJoints, "position.right", "rad") == -0.5 &&
                field(mappedJoints, "velocity.left", "rad/s") == 1.5 &&
                field(mappedJoints, "effort.right") == 5.0,
            "JointState arrays map by joint name and available length");

    CdrWriter odometry;
    odometry.header(7, 8, "odom");
    odometry.string("base_link");
    for (const auto value : {3.5, 2.5, 1.5, 0.0, 0.0, 0.0, 1.0}) {
        odometry.float64(value);
    }
    odometry.fixedDoubles(36);
    for (const auto value : {1.0, -4.5, 3.0, 0.1, 0.2, 0.3}) {
        odometry.float64(value);
    }
    odometry.fixedDoubles(36);
    const auto mappedOdometry = lab::adapters::rosbag2::mapStructuredCdrFields(
        "nav_msgs/msg/Odometry", odometry.bytes());
    require(mappedOdometry.success && mappedOdometry.fields.size() == 13 &&
                mappedOdometry.sourceTimestamp == 7'000'000'008LL &&
                field(mappedOdometry, "pose.pose.position.x", "m") == 3.5 &&
                field(mappedOdometry, "twist.twist.linear.y", "m/s") == -4.5,
            "Odometry maps nested pose and twist while skipping covariance");
}

void testSafeFallbacks() {
    require(!lab::adapters::rosbag2::hasStructuredCdrMapping(
                "custom_msgs/msg/Unknown"),
            "unknown types are advertised as raw-only");
    const auto unknown = lab::adapters::rosbag2::mapStructuredCdrFields(
        "custom_msgs/msg/Unknown", std::vector<std::uint8_t>{0, 1, 0, 0});
    require(!unknown.supported && !unknown.success && unknown.fields.empty(),
            "unknown types never produce invented fields");

    const auto truncated = lab::adapters::rosbag2::mapStructuredCdrFields(
        "std_msgs/msg/Float64", std::vector<std::uint8_t>{0, 1, 0, 0, 1});
    require(truncated.supported && !truncated.success && truncated.fields.empty() &&
                truncated.warning.find("truncated") != std::string::npos,
            "truncated built-in CDR falls back without partial fields");

    const auto representation = lab::adapters::rosbag2::mapStructuredCdrFields(
        "std_msgs/msg/Float64", std::vector<std::uint8_t>{0, 7, 0, 0, 0, 0, 0, 0});
    require(!representation.success &&
                representation.warning.find("encapsulation") != std::string::npos,
            "unsupported CDR representation is rejected explicitly");
}

}  // namespace

int main() {
    try {
        testScalarAndEndian();
        testStampedAndSensorTypes();
        testVariableAndNestedTypes();
        testSafeFallbacks();
        std::cout << "Lab Debugger CDR field mapper tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Lab Debugger CDR field mapper tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}

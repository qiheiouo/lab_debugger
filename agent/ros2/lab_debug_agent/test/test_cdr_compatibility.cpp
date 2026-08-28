#include "lab/adapters/rosbag2/cdr_field_mapper.hpp"
#include "lab_debug_agent/field_mapper.hpp"

#include <geometry_msgs/msg/pose_stamped.hpp>
#include <geometry_msgs/msg/twist.hpp>
#include <geometry_msgs/msg/twist_stamped.hpp>
#include <nav_msgs/msg/odometry.hpp>
#include <rclcpp/serialization.hpp>
#include <sensor_msgs/msg/imu.hpp>
#include <sensor_msgs/msg/joint_state.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/float32.hpp>
#include <std_msgs/msg/float64.hpp>
#include <std_msgs/msg/int32.hpp>
#include <std_msgs/msg/u_int32.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using lab::adapters::rosbag2::CdrMappingResult;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool closeEnough(double actual, double expected) {
    return std::abs(actual - expected) <=
           1e-12 * std::max({1.0, std::abs(actual), std::abs(expected)});
}

template<typename Message>
rclcpp::SerializedMessage serialize(const Message& message) {
    rclcpp::SerializedMessage serialized;
    rclcpp::Serialization<Message> serialization;
    serialization.serialize_message(&message, &serialized);
    return serialized;
}

std::vector<std::uint8_t> bytesOf(const rclcpp::SerializedMessage& serialized) {
    const auto& raw = serialized.get_rcl_serialized_message();
    require(raw.buffer != nullptr && raw.buffer_length >= 4,
            "Humble serialization includes a CDR encapsulation header");
    return {raw.buffer, raw.buffer + raw.buffer_length};
}

double agentNumber(const lab::core::agent::FieldValue& field) {
    if (const auto* number = std::get_if<double>(&field.value)) return *number;
    if (const auto* boolean = std::get_if<bool>(&field.value)) {
        return *boolean ? 1.0 : 0.0;
    }
    throw std::runtime_error("Agent field is not numeric: " + field.path);
}

void compareWithAgent(std::string_view type,
                      const rclcpp::SerializedMessage& serialized,
                      const CdrMappingResult& mapped) {
    const auto agent = lab_debug_agent::mapSerializedFields(
        std::string(type), serialized);
    require(agent.warning.empty(), "Agent maps the same valid CDR: " + std::string(type));
    require(agent.sourceTimestamp == mapped.sourceTimestamp,
            "Header timestamp matches Agent: " + std::string(type));

    std::size_t numericCount = 0;
    for (const auto& field : agent.fields) {
        if (!std::holds_alternative<std::string>(field.value)) ++numericCount;
    }
    require(numericCount == mapped.fields.size(),
            "numeric field count matches Agent: " + std::string(type));
    for (const auto& field : mapped.fields) {
        const auto item = std::find_if(
            agent.fields.begin(), agent.fields.end(), [&](const auto& candidate) {
                return candidate.path == field.path;
            });
        require(item != agent.fields.end(),
                "field path matches Agent: " + std::string(type) + "." + field.path);
        require(item->unit == field.unit,
                "field unit matches Agent: " + std::string(type) + "." + field.path);
        require(closeEnough(agentNumber(*item), field.value),
                "field value matches Agent: " + std::string(type) + "." + field.path);
    }
}

CdrMappingResult mapRealCdr(std::string_view type,
                            const rclcpp::SerializedMessage& serialized) {
    auto bytes = bytesOf(serialized);
    const auto original = bytes;
    const auto representation = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[0]) << 8U |
        static_cast<std::uint16_t>(bytes[1]));
    require(representation == 0x0000U || representation == 0x0001U,
            "Humble produced a supported CDR1 representation");
    require(bytes[2] == 0 && bytes[3] == 0,
            "Humble produced the expected zero encapsulation options");
    static bool reportedEncapsulation = false;
    if (!reportedEncapsulation) {
        std::cout << "Observed Humble CDR encapsulation:";
        for (std::size_t index = 0; index < 4; ++index) {
            std::cout << ' ' << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned>(bytes[index]);
        }
        std::cout << std::dec << std::setfill(' ') << '\n';
        reportedEncapsulation = true;
    }

    const auto mapped = lab::adapters::rosbag2::mapStructuredCdrFields(type, bytes);
    require(mapped.supported && mapped.success && mapped.warning.empty(),
            "ROS-independent mapper accepts Humble CDR for " + std::string(type) +
                (mapped.warning.empty() ? std::string{} : ": " + mapped.warning));
    require(bytes == original,
            "ROS-independent mapper does not modify raw CDR for " + std::string(type));
    compareWithAgent(type, serialized, mapped);
    return mapped;
}

const lab::adapters::rosbag2::CdrNumericField& field(
    const CdrMappingResult& mapped,
    std::string_view path,
    std::string_view unit = {}) {
    const auto item = std::find_if(
        mapped.fields.begin(), mapped.fields.end(), [&](const auto& candidate) {
            return candidate.path == path;
        });
    require(item != mapped.fields.end(), "mapped field exists: " + std::string(path));
    require(item->unit == unit, "mapped field unit is exact: " + std::string(path));
    return *item;
}

void expect(const CdrMappingResult& mapped,
            std::string_view path,
            double value,
            std::string_view unit = {}) {
    require(closeEnough(field(mapped, path, unit).value, value),
            "mapped field value is exact: " + std::string(path));
}

void testScalars() {
    std_msgs::msg::Float32 float32;
    float32.data = -12.5F;
    expect(mapRealCdr("std_msgs/msg/Float32", serialize(float32)), "data", -12.5);

    std_msgs::msg::Float64 float64;
    float64.data = 42.125;
    expect(mapRealCdr("std_msgs/msg/Float64", serialize(float64)), "data", 42.125);

    std_msgs::msg::Int32 int32;
    int32.data = -2'000'000'001;
    expect(mapRealCdr("std_msgs/msg/Int32", serialize(int32)), "data", int32.data);

    std_msgs::msg::UInt32 uint32;
    uint32.data = 4'000'000'001U;
    expect(mapRealCdr("std_msgs/msg/UInt32", serialize(uint32)), "data", uint32.data);

    std_msgs::msg::Bool boolean;
    boolean.data = true;
    expect(mapRealCdr("std_msgs/msg/Bool", serialize(boolean)), "data", 1.0);
}

void testGeometry() {
    geometry_msgs::msg::Twist twist;
    twist.linear.x = 1.25;
    twist.linear.y = -2.5;
    twist.linear.z = 3.75;
    twist.angular.x = -0.125;
    twist.angular.y = 0.25;
    twist.angular.z = 0.75;
    const auto mappedTwist = mapRealCdr("geometry_msgs/msg/Twist", serialize(twist));
    require(mappedTwist.fields.size() == 6, "Twist maps all six fields");
    expect(mappedTwist, "linear.x", twist.linear.x, "m/s");
    expect(mappedTwist, "linear.z", twist.linear.z, "m/s");
    expect(mappedTwist, "angular.y", twist.angular.y, "rad/s");

    geometry_msgs::msg::TwistStamped stamped;
    stamped.header.stamp.sec = 12;
    stamped.header.stamp.nanosec = 345;
    stamped.header.frame_id = "base_link";
    stamped.twist = twist;
    const auto mappedStamped =
        mapRealCdr("geometry_msgs/msg/TwistStamped", serialize(stamped));
    require(mappedStamped.sourceTimestamp == 12'000'000'345LL,
            "TwistStamped Header converts to exact nanoseconds");
    expect(mappedStamped, "twist.linear.y", twist.linear.y, "m/s");
    expect(mappedStamped, "twist.angular.z", twist.angular.z, "rad/s");

    geometry_msgs::msg::PoseStamped pose;
    pose.header.stamp.sec = 23;
    pose.header.stamp.nanosec = 456;
    pose.header.frame_id = "map";
    pose.pose.position.x = 8.5;
    pose.pose.position.y = -7.25;
    pose.pose.position.z = 0.125;
    pose.pose.orientation.x = 0.1;
    pose.pose.orientation.y = 0.2;
    pose.pose.orientation.z = 0.3;
    pose.pose.orientation.w = 0.9;
    const auto mappedPose =
        mapRealCdr("geometry_msgs/msg/PoseStamped", serialize(pose));
    require(mappedPose.sourceTimestamp == 23'000'000'456LL,
            "PoseStamped Header converts to exact nanoseconds");
    expect(mappedPose, "pose.position.y", pose.pose.position.y, "m");
    expect(mappedPose, "pose.orientation.w", pose.pose.orientation.w);

    pose.header.stamp.sec = 0;
    pose.header.stamp.nanosec = 0;
    const auto zeroHeader =
        mapRealCdr("geometry_msgs/msg/PoseStamped", serialize(pose));
    require(zeroHeader.sourceTimestamp == 0,
            "zero Header remains zero for bag timestamp fallback");
}

void testSensors() {
    sensor_msgs::msg::Imu imu;
    imu.header.stamp.sec = 34;
    imu.header.stamp.nanosec = 567;
    imu.header.frame_id = "imu_link";
    imu.orientation.x = 0.01;
    imu.orientation.y = 0.02;
    imu.orientation.z = 0.03;
    imu.orientation.w = 0.99;
    for (std::size_t index = 0; index < imu.orientation_covariance.size(); ++index) {
        imu.orientation_covariance[index] = 100.0 + static_cast<double>(index);
        imu.angular_velocity_covariance[index] = 200.0 + static_cast<double>(index);
        imu.linear_acceleration_covariance[index] = 300.0 + static_cast<double>(index);
    }
    imu.angular_velocity.x = -1.5;
    imu.angular_velocity.y = 2.5;
    imu.angular_velocity.z = -3.5;
    imu.linear_acceleration.x = 9.81;
    imu.linear_acceleration.y = -0.25;
    imu.linear_acceleration.z = -9.81;
    const auto mappedImu = mapRealCdr("sensor_msgs/msg/Imu", serialize(imu));
    require(mappedImu.fields.size() == 10 &&
                mappedImu.sourceTimestamp == 34'000'000'567LL,
            "Imu maps Header and ten semantic fields");
    expect(mappedImu, "orientation.w", imu.orientation.w);
    expect(mappedImu, "angular_velocity.z", imu.angular_velocity.z, "rad/s");
    expect(mappedImu, "linear_acceleration.x", imu.linear_acceleration.x, "m/s^2");

    sensor_msgs::msg::JointState joints;
    joints.header.stamp.sec = 45;
    joints.header.stamp.nanosec = 678;
    joints.header.frame_id = "arm";
    joints.name = {"left", "right"};
    joints.position = {0.25, -0.5};
    joints.velocity = {1.5, -2.5};
    joints.effort = {4.0, 5.0};
    const auto mappedJoints =
        mapRealCdr("sensor_msgs/msg/JointState", serialize(joints));
    require(mappedJoints.fields.size() == 6 &&
                mappedJoints.sourceTimestamp == 45'000'000'678LL,
            "JointState maps all same-length sequences");
    expect(mappedJoints, "position.right", -0.5, "rad");
    expect(mappedJoints, "velocity.left", 1.5, "rad/s");
    expect(mappedJoints, "effort.right", 5.0);
}

void testOdometry() {
    nav_msgs::msg::Odometry odometry;
    odometry.header.stamp.sec = 56;
    odometry.header.stamp.nanosec = 789;
    odometry.header.frame_id = "odom";
    odometry.child_frame_id = "base_link";
    odometry.pose.pose.position.x = 3.5;
    odometry.pose.pose.position.y = -2.5;
    odometry.pose.pose.position.z = 1.5;
    odometry.pose.pose.orientation.w = 1.0;
    for (std::size_t index = 0; index < odometry.pose.covariance.size(); ++index) {
        odometry.pose.covariance[index] = 400.0 + static_cast<double>(index);
        odometry.twist.covariance[index] = 500.0 + static_cast<double>(index);
    }
    odometry.twist.twist.linear.x = 1.0;
    odometry.twist.twist.linear.y = -4.5;
    odometry.twist.twist.linear.z = 3.0;
    odometry.twist.twist.angular.x = 0.1;
    odometry.twist.twist.angular.y = 0.2;
    odometry.twist.twist.angular.z = 0.3;
    const auto mapped = mapRealCdr("nav_msgs/msg/Odometry", serialize(odometry));
    require(mapped.fields.size() == 13 &&
                mapped.sourceTimestamp == 56'000'000'789LL,
            "Odometry maps Header and thirteen semantic fields");
    expect(mapped, "pose.pose.position.x", 3.5, "m");
    expect(mapped, "pose.pose.orientation.w", 1.0);
    expect(mapped, "twist.twist.linear.y", -4.5, "m/s");
    expect(mapped, "twist.twist.angular.z", 0.3, "rad/s");
}

}  // namespace

int main() {
    try {
        testScalars();
        testGeometry();
        testSensors();
        testOdometry();
        std::cout << "Humble real CDR compatibility tests passed "
                  << "(encapsulation, alignment, fields, units, timestamps, raw bytes).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Humble real CDR compatibility tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}

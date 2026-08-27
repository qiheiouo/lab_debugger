#include "lab_debug_agent/field_mapper.hpp"

#include <geometry_msgs/msg/point.hpp>
#include <geometry_msgs/msg/twist.hpp>
#include <geometry_msgs/msg/twist_stamped.hpp>
#include <nav_msgs/msg/odometry.hpp>
#include <rclcpp/serialization.hpp>
#include <sensor_msgs/msg/imu.hpp>
#include <sensor_msgs/msg/joint_state.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/float64.hpp>
#include <std_msgs/msg/string.hpp>

#include <iostream>
#include <stdexcept>
#include <string>
#include <variant>

namespace {

using lab::core::agent::FieldValue;
using lab_debug_agent::MappedFields;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("field mapper: " + message);
}

template<typename Message>
rclcpp::SerializedMessage serialize(const Message& message) {
    rclcpp::SerializedMessage serialized;
    rclcpp::Serialization<Message> serialization;
    serialization.serialize_message(&message, &serialized);
    return serialized;
}

const FieldValue& field(const MappedFields& mapped, const std::string& path) {
    for (const auto& value : mapped.fields) {
        if (value.path == path) return value;
    }
    throw std::runtime_error("field mapper: missing field " + path);
}

double number(const MappedFields& mapped, const std::string& path) {
    const auto* value = std::get_if<double>(&field(mapped, path).value);
    require(value != nullptr, path + " retains numeric type");
    return *value;
}

void testScalarTypes() {
    std_msgs::msg::Float64 floatMessage;
    floatMessage.data = 42.125;
    const auto mappedFloat = lab_debug_agent::mapSerializedFields(
        "std_msgs/msg/Float64", serialize(floatMessage));
    require(number(mappedFloat, "data") == 42.125, "Float64 data maps exactly");

    std_msgs::msg::Bool boolMessage;
    boolMessage.data = true;
    const auto mappedBool = lab_debug_agent::mapSerializedFields(
        "std_msgs/msg/Bool", serialize(boolMessage));
    const auto* boolean = std::get_if<bool>(&field(mappedBool, "data").value);
    require(boolean && *boolean, "Bool data retains boolean type and value");

    std_msgs::msg::String stringMessage;
    stringMessage.data = "Humble 字符串";
    const auto mappedString = lab_debug_agent::mapSerializedFields(
        "std_msgs/msg/String", serialize(stringMessage));
    const auto* text = std::get_if<std::string>(&field(mappedString, "data").value);
    require(text && *text == stringMessage.data,
            "String data retains UTF-8 text type and value");
}

void testGeometryAndHeaderTimestamp() {
    geometry_msgs::msg::Twist twist;
    twist.linear.x = 1.25;
    twist.linear.y = -2.5;
    twist.angular.z = 0.75;
    const auto mappedTwist = lab_debug_agent::mapSerializedFields(
        "geometry_msgs/msg/Twist", serialize(twist));
    require(mappedTwist.fields.size() == 6, "Twist maps six vector components");
    require(number(mappedTwist, "linear.x") == 1.25 &&
                number(mappedTwist, "linear.y") == -2.5 &&
                number(mappedTwist, "angular.z") == 0.75,
            "Twist numeric values map exactly");

    geometry_msgs::msg::TwistStamped stamped;
    stamped.header.stamp.sec = 123;
    stamped.header.stamp.nanosec = 456;
    stamped.header.frame_id = "base_link";
    stamped.twist.angular.x = -0.5;
    const auto mappedStamped = lab_debug_agent::mapSerializedFields(
        "geometry_msgs/msg/TwistStamped", serialize(stamped));
    require(mappedStamped.sourceTimestamp == 123'000'000'456LL,
            "TwistStamped header becomes source timestamp");
    const auto* frameId =
        std::get_if<std::string>(&field(mappedStamped, "header.frame_id").value);
    require(frameId && *frameId == "base_link", "header frame id retains text type");
    require(number(mappedStamped, "twist.angular.x") == -0.5,
            "TwistStamped nested field maps exactly");

    stamped.header.stamp.sec = 0;
    stamped.header.stamp.nanosec = 0;
    const auto zeroStamped = lab_debug_agent::mapSerializedFields(
        "geometry_msgs/msg/TwistStamped", serialize(stamped));
    require(zeroStamped.sourceTimestamp == 0,
            "zero ROS header remains zero for ServerSession fallback");
}

void testSensorAndOdometryTypes() {
    sensor_msgs::msg::Imu imu;
    imu.header.stamp.sec = 9;
    imu.header.frame_id = "imu_link";
    imu.orientation.w = 0.9;
    imu.angular_velocity.z = -1.5;
    imu.linear_acceleration.x = 9.81;
    const auto mappedImu = lab_debug_agent::mapSerializedFields(
        "sensor_msgs/msg/Imu", serialize(imu));
    require(mappedImu.sourceTimestamp == 9'000'000'000LL &&
                number(mappedImu, "orientation.w") == 0.9 &&
                number(mappedImu, "angular_velocity.z") == -1.5 &&
                number(mappedImu, "linear_acceleration.x") == 9.81,
            "Imu header and structured values map exactly");

    sensor_msgs::msg::JointState joints;
    joints.name = {"left", "right"};
    joints.position = {0.1, 0.2};
    joints.velocity = {1.1, 1.2};
    joints.effort = {2.1, 2.2};
    const auto mappedJoints = lab_debug_agent::mapSerializedFields(
        "sensor_msgs/msg/JointState", serialize(joints));
    require(number(mappedJoints, "position.left") == 0.1 &&
                number(mappedJoints, "velocity.right") == 1.2 &&
                number(mappedJoints, "effort.left") == 2.1,
            "JointState arrays map by joint name");

    nav_msgs::msg::Odometry odometry;
    odometry.header.stamp.sec = 7;
    odometry.header.frame_id = "odom";
    odometry.child_frame_id = "base_link";
    odometry.pose.pose.position.x = 3.5;
    odometry.twist.twist.linear.y = -4.5;
    const auto mappedOdometry = lab_debug_agent::mapSerializedFields(
        "nav_msgs/msg/Odometry", serialize(odometry));
    require(mappedOdometry.sourceTimestamp == 7'000'000'000LL &&
                number(mappedOdometry, "pose.pose.position.x") == 3.5 &&
                number(mappedOdometry, "twist.twist.linear.y") == -4.5,
            "Odometry nested pose and twist map exactly");
}

void testUnknownAndMalformedTypes() {
    geometry_msgs::msg::Point point;
    point.x = 8.0;
    const auto unknown = lab_debug_agent::mapSerializedFields(
        "geometry_msgs/msg/Point", serialize(point));
    require(unknown.fields.empty() && unknown.warning.empty(),
            "unmapped but valid message has no fabricated fields or warning");

    auto malformed = serialize(std_msgs::msg::Float64{});
    malformed.get_rcl_serialized_message().buffer_length = 1;
    const auto failed = lab_debug_agent::mapSerializedFields(
        "std_msgs/msg/Float64", malformed);
    require(failed.fields.empty() && failed.sourceTimestamp == 0 &&
                failed.warning.find("Structured field mapping failed") !=
                    std::string::npos,
            "deserialization failure clears fields and returns a clear warning");
}

}  // namespace

int main() {
    try {
        testScalarTypes();
        testGeometryAndHeaderTimestamp();
        testSensorAndOdometryTypes();
        testUnknownAndMalformedTypes();
        std::cout << "All ROS field mapper tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Test failed: " << exception.what() << '\n';
        return 1;
    }
}

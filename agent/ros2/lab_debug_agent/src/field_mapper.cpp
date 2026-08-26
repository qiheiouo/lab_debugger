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
#include <std_msgs/msg/string.hpp>
#include <std_msgs/msg/u_int32.hpp>

#include <algorithm>
#include <exception>
#include <utility>

namespace lab_debug_agent {
namespace {

using FieldValue = lab::core::agent::FieldValue;

template<typename Message>
Message deserialize(const rclcpp::SerializedMessage& serialized) {
    Message message;
    rclcpp::Serialization<Message> serialization;
    serialization.deserialize_message(&serialized, &message);
    return message;
}

template<typename Stamp>
lab::core::Timestamp timestamp(const Stamp& stamp) {
    return static_cast<lab::core::Timestamp>(stamp.sec) * 1'000'000'000LL +
           static_cast<lab::core::Timestamp>(stamp.nanosec);
}

void vector3(
    std::vector<FieldValue>& fields,
    const std::string& prefix,
    const geometry_msgs::msg::Vector3& value,
    const std::string& unit) {
    fields.push_back({prefix + ".x", unit, value.x});
    fields.push_back({prefix + ".y", unit, value.y});
    fields.push_back({prefix + ".z", unit, value.z});
}

void quaternion(
    std::vector<FieldValue>& fields,
    const std::string& prefix,
    const geometry_msgs::msg::Quaternion& value) {
    fields.push_back({prefix + ".x", "", value.x});
    fields.push_back({prefix + ".y", "", value.y});
    fields.push_back({prefix + ".z", "", value.z});
    fields.push_back({prefix + ".w", "", value.w});
}

void pose(
    std::vector<FieldValue>& fields,
    const std::string& prefix,
    const geometry_msgs::msg::Pose& value) {
    fields.push_back({prefix + ".position.x", "m", value.position.x});
    fields.push_back({prefix + ".position.y", "m", value.position.y});
    fields.push_back({prefix + ".position.z", "m", value.position.z});
    quaternion(fields, prefix + ".orientation", value.orientation);
}

void twist(
    std::vector<FieldValue>& fields,
    const std::string& prefix,
    const geometry_msgs::msg::Twist& value) {
    vector3(fields, prefix + ".linear", value.linear, "m/s");
    vector3(fields, prefix + ".angular", value.angular, "rad/s");
}

}  // namespace

MappedFields mapSerializedFields(
    const std::string& type,
    const rclcpp::SerializedMessage& serialized) {
    MappedFields result;
    try {
        if (type == "std_msgs/msg/Float64") {
            result.fields.push_back({"data", "", deserialize<std_msgs::msg::Float64>(serialized).data});
        } else if (type == "std_msgs/msg/Float32") {
            result.fields.push_back({"data", "", static_cast<double>(
                deserialize<std_msgs::msg::Float32>(serialized).data)});
        } else if (type == "std_msgs/msg/Int32") {
            result.fields.push_back({"data", "", static_cast<double>(
                deserialize<std_msgs::msg::Int32>(serialized).data)});
        } else if (type == "std_msgs/msg/UInt32") {
            result.fields.push_back({"data", "", static_cast<double>(
                deserialize<std_msgs::msg::UInt32>(serialized).data)});
        } else if (type == "std_msgs/msg/Bool") {
            result.fields.push_back({"data", "", deserialize<std_msgs::msg::Bool>(serialized).data});
        } else if (type == "std_msgs/msg/String") {
            result.fields.push_back({"data", "", deserialize<std_msgs::msg::String>(serialized).data});
        } else if (type == "geometry_msgs/msg/Twist") {
            twist(result.fields, "", deserialize<geometry_msgs::msg::Twist>(serialized));
            for (auto& field : result.fields) {
                if (!field.path.empty() && field.path.front() == '.') field.path.erase(0, 1);
            }
        } else if (type == "geometry_msgs/msg/TwistStamped") {
            const auto message = deserialize<geometry_msgs::msg::TwistStamped>(serialized);
            result.sourceTimestamp = timestamp(message.header.stamp);
            result.fields.push_back({"header.frame_id", "", message.header.frame_id});
            twist(result.fields, "twist", message.twist);
        } else if (type == "geometry_msgs/msg/PoseStamped") {
            const auto message = deserialize<geometry_msgs::msg::PoseStamped>(serialized);
            result.sourceTimestamp = timestamp(message.header.stamp);
            result.fields.push_back({"header.frame_id", "", message.header.frame_id});
            pose(result.fields, "pose", message.pose);
        } else if (type == "sensor_msgs/msg/Imu") {
            const auto message = deserialize<sensor_msgs::msg::Imu>(serialized);
            result.sourceTimestamp = timestamp(message.header.stamp);
            result.fields.push_back({"header.frame_id", "", message.header.frame_id});
            quaternion(result.fields, "orientation", message.orientation);
            vector3(result.fields, "angular_velocity", message.angular_velocity, "rad/s");
            vector3(result.fields, "linear_acceleration", message.linear_acceleration, "m/s^2");
        } else if (type == "sensor_msgs/msg/JointState") {
            const auto message = deserialize<sensor_msgs::msg::JointState>(serialized);
            result.sourceTimestamp = timestamp(message.header.stamp);
            const auto count = std::min(message.name.size(), message.position.size());
            for (std::size_t index = 0; index < count; ++index) {
                result.fields.push_back(
                    {"position." + message.name[index], "rad", message.position[index]});
            }
            for (std::size_t index = 0;
                 index < std::min(message.name.size(), message.velocity.size()); ++index) {
                result.fields.push_back(
                    {"velocity." + message.name[index], "rad/s", message.velocity[index]});
            }
            for (std::size_t index = 0;
                 index < std::min(message.name.size(), message.effort.size()); ++index) {
                result.fields.push_back(
                    {"effort." + message.name[index], "", message.effort[index]});
            }
        } else if (type == "nav_msgs/msg/Odometry") {
            const auto message = deserialize<nav_msgs::msg::Odometry>(serialized);
            result.sourceTimestamp = timestamp(message.header.stamp);
            result.fields.push_back({"header.frame_id", "", message.header.frame_id});
            result.fields.push_back({"child_frame_id", "", message.child_frame_id});
            pose(result.fields, "pose.pose", message.pose.pose);
            twist(result.fields, "twist.twist", message.twist.twist);
        }
    } catch (const std::exception& exception) {
        result.fields.clear();
        result.sourceTimestamp = 0;
        result.warning = "Structured field mapping failed for " + type + ": " + exception.what();
    }
    return result;
}

}  // namespace lab_debug_agent

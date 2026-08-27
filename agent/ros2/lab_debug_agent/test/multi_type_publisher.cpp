#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/bool.hpp>

#include <chrono>
#include <memory>
#include <stdexcept>

using namespace std::chrono_literals;

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    try {
        if (argc != 2) throw std::invalid_argument("expected topic name");
        auto node = std::make_shared<rclcpp::Node>(
            "lab_debug_agent_multi_type_publisher");
        rclcpp::QoS qos(10);
        qos.best_effort();
        auto publisher = node->create_publisher<std_msgs::msg::Bool>(argv[1], qos);
        auto timer = node->create_wall_timer(100ms, [publisher] {
            std_msgs::msg::Bool message;
            message.data = true;
            publisher->publish(message);
        });
        rclcpp::spin(node);
        timer.reset();
        publisher.reset();
        node.reset();
        rclcpp::shutdown();
        return 0;
    } catch (const std::exception&) {
        rclcpp::shutdown();
        return 1;
    }
}

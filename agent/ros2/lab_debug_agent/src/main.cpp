#include "lab_debug_agent/ros_agent_node.hpp"

#include <rclcpp/rclcpp.hpp>

#include <exception>
#include <memory>

int main(int argc, char* argv[]) {
    rclcpp::init(argc, argv);
    try {
        auto node = std::make_shared<lab_debug_agent::RosAgentNode>();
        rclcpp::executors::MultiThreadedExecutor executor;
        executor.add_node(node);
        executor.spin();
        executor.remove_node(node);
        node.reset();
        rclcpp::shutdown();
        return 0;
    } catch (const std::exception& exception) {
        RCLCPP_FATAL(rclcpp::get_logger("lab_debug_agent"), "%s", exception.what());
        rclcpp::shutdown();
        return 1;
    }
}

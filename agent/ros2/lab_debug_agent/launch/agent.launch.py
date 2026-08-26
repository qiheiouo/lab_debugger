from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node
from launch_ros.parameter_descriptions import ParameterValue
from ament_index_python.packages import get_package_share_directory
from pathlib import Path


def generate_launch_description():
    config = Path(get_package_share_directory("lab_debug_agent")) / "config" / "agent.yaml"
    return LaunchDescription([
        DeclareLaunchArgument("bind_address", default_value="127.0.0.1"),
        DeclareLaunchArgument("port", default_value="9750"),
        DeclareLaunchArgument("agent_id", default_value="lab-agent"),
        Node(
            package="lab_debug_agent",
            executable="lab_debug_agent_node",
            name="lab_debug_agent",
            output="screen",
            parameters=[
                str(config),
                {
                    "bind_address": LaunchConfiguration("bind_address"),
                    "port": ParameterValue(LaunchConfiguration("port"), value_type=int),
                    "agent_id": LaunchConfiguration("agent_id"),
                },
            ],
        )
    ])

cmake_minimum_required(VERSION 3.8)
project(lab_debug_agent VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 20)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(nav_msgs REQUIRED)

get_filename_component(LAB_DEBUGGER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

add_library(lab_agent_protocol_core STATIC
  "${LAB_DEBUGGER_ROOT}/src/core/src/agent_protocol.cpp"
  "${LAB_DEBUGGER_ROOT}/src/core/src/remote_agent_session.cpp"
)
target_include_directories(lab_agent_protocol_core PUBLIC
  "${LAB_DEBUGGER_ROOT}/src/core/include"
)
target_compile_features(lab_agent_protocol_core PUBLIC cxx_std_20)

add_executable(lab_debug_agent_node
  src/field_mapper.cpp
  src/main.cpp
  src/ros_agent_node.cpp
  src/tcp_server.cpp
)
target_include_directories(lab_debug_agent_node PRIVATE include)
target_link_libraries(lab_debug_agent_node PRIVATE lab_agent_protocol_core)
ament_target_dependencies(lab_debug_agent_node
  rclcpp
  std_msgs
  geometry_msgs
  sensor_msgs
  nav_msgs
)
target_compile_options(lab_debug_agent_node PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS lab_debug_agent_node
  DESTINATION lib/${PROJECT_NAME}
)
install(DIRECTORY config launch
  DESTINATION share/${PROJECT_NAME}
)

ament_package()

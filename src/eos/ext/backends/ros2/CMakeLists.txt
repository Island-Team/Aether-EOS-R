# ROS 2 backend — the only target in the repository with a ROS 2 dependency.
# Build only when a ROS 2 environment is sourced: -DEOS_BACKEND_ROS2=ON.
# Standard common interfaces only; vendor message types live in the
# deployment's graph-edge adapter, never here.
add_library(eos_backend_ros2
  ros2_backend.cpp
)

target_include_directories(eos_backend_ros2
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(eos_backend_ros2
  PRIVATE eos_internal eos_lib
)

find_package(rclcpp REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(std_msgs REQUIRED)
find_package(PNG REQUIRED)

target_link_libraries(eos_backend_ros2
  PRIVATE rclcpp::rclcpp
          geometry_msgs::geometry_msgs__rosidl_typesupport_cpp
          sensor_msgs::sensor_msgs__rosidl_typesupport_cpp
          std_msgs::std_msgs__rosidl_typesupport_cpp
          PNG::PNG
)

if(ISLAND_BUILD_TESTS)
  add_subdirectory(tests)
endif()

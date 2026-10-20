find_package(GTest REQUIRED)
find_package(rclcpp REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(std_msgs REQUIRED)
add_executable(eos_ros2_tests ros2_backend_test.cpp)
target_link_libraries(eos_ros2_tests PRIVATE
  eos_internal eos_backend_ros2 eos_backend_demo_cam GTest::gtest_main
  rclcpp::rclcpp
  geometry_msgs::geometry_msgs__rosidl_typesupport_cpp
  sensor_msgs::sensor_msgs__rosidl_typesupport_cpp
  std_msgs::std_msgs__rosidl_typesupport_cpp)
add_test(NAME eos_ros2_tests COMMAND eos_ros2_tests)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ros-logs")
set_tests_properties(eos_ros2_tests PROPERTIES
  ENVIRONMENT
    "ROS_LOCALHOST_ONLY=1;ROS_DOMAIN_ID=231;ROS_LOG_DIR=${CMAKE_CURRENT_BINARY_DIR}/ros-logs"
  TIMEOUT 30
  RUN_SERIAL TRUE)

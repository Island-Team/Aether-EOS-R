# The EOS-facing socket client has no ROS dependency. The optional bridge is
# the only binary that links ROS middleware and hosts graph-side transport.
add_library(eos_backend_socket
  socket_backend.cpp
  socket_protocol.cpp
)
target_link_libraries(eos_backend_socket PUBLIC eos_internal nlohmann_json::nlohmann_json)

if(EOS_BACKEND_ROS2)
  find_package(rclcpp REQUIRED)
  find_package(geometry_msgs REQUIRED)
  find_package(sensor_msgs REQUIRED)
  find_package(std_msgs REQUIRED)
  find_package(PNG REQUIRED)
  add_library(eos_backend_ros2 EXCLUDE_FROM_ALL ros2_backend.cpp)
  target_link_libraries(eos_backend_ros2 PRIVATE eos_internal eos_lib
    rclcpp::rclcpp geometry_msgs::geometry_msgs__rosidl_typesupport_cpp
    sensor_msgs::sensor_msgs__rosidl_typesupport_cpp
    std_msgs::std_msgs__rosidl_typesupport_cpp PNG::PNG)
  add_executable(eos-ros2-bridge EXCLUDE_FROM_ALL bridge_main.cpp)
  target_link_libraries(eos-ros2-bridge PRIVATE eos_internal eos_hardware
    eos_backend_ros2 eos_backend_socket eos_lib nlohmann_json::nlohmann_json)
  # The bridge is an external package artifact. Enabling the toolchain makes
  # its target available, but a normal EOS build does not produce it.
  add_custom_target(island_pkg_ros2_backend DEPENDS eos-ros2-bridge)
  install(TARGETS eos-ros2-bridge RUNTIME DESTINATION bin
    COMPONENT island_pkg_ros2_backend EXCLUDE_FROM_ALL)
  if(ISLAND_BUILD_TESTS)
    add_subdirectory(tests)
  endif()
endif()

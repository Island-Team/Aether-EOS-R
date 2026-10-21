# South-bound replaceable adapters behind eos/hardware/backend.h.
# Each adapter is admitted by runtime/hardware and re-admitted on hot-plug.
option(EOS_BACKEND_ROS2 "Build the ROS 2 backend (requires a sourced ROS 2 environment)" OFF)

add_subdirectory(demo)
add_subdirectory(aplay)
add_subdirectory(alsa)
add_subdirectory(v4l2)

if(EOS_BACKEND_ROS2)
  add_subdirectory(ros2)
endif()

if(ISLAND_BUILD_TESTS)
  add_subdirectory(tests)
endif()

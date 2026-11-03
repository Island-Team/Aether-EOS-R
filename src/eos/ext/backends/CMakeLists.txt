# Southbound adapters behind eos/hardware/backend.h. Their descriptors own
# adapter settings, capabilities, validation, and construction.
option(EOS_BACKEND_ROS2 "Build the ROS 2 backend (requires a sourced ROS 2 environment)" OFF)
option(EOS_BACKEND_VENDOR "Build the private Island Base vendor backend" OFF)

add_subdirectory(vendor/audio/aplay)
add_subdirectory(vendor/audio/alsa)
add_subdirectory(vendor/camera/v4l2)

if(EOS_BACKEND_VENDOR)
  add_subdirectory(vendor)
endif()

add_subdirectory(ros2)

if(ISLAND_BUILD_TESTS)
  add_subdirectory(tests)
endif()

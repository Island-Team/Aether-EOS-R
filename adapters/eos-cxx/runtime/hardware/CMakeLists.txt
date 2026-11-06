# runtime/hardware — generic binding parsing, catalog-driven admission, tick,
# and health aggregation. The required demo adapter lives here; optional
# transport descriptors remain under ext/backends.
add_library(eos_hardware
  manager/backend_catalog.cpp
  manager/capability_bindings.cpp
  backends/demo/descriptor.cpp
  ../../ext/backends/mujoco/descriptor.cpp
  backends/simulator/descriptor.cpp
  ../../ext/backends/ros2/descriptor.cpp
  ../../ext/backends/vendor/descriptor.cpp
  ../../ext/backends/vendor/audio/aplay/descriptor.cpp
  ../../ext/backends/vendor/audio/alsa/descriptor.cpp
  ../../ext/backends/vendor/camera/v4l2/descriptor.cpp
  admission.cpp
  arm.cpp
  base.cpp
  binding.cpp
  catalog.cpp
  camera.cpp
  gimbal.cpp
  gripper.cpp
  identity.cpp
  mic.cpp
  mode.cpp
  registry.cpp
  speaker.cpp
  telemetry_hub.cpp
)

target_include_directories(eos_hardware
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

# Backend libraries stay private to the hardware target.
target_link_libraries(eos_hardware
  PUBLIC tomlplusplus::tomlplusplus
  PRIVATE eos_internal
          eos_backend_aplay
          eos_backend_alsa
          eos_backend_v4l2
          eos_lib
          eos_backend_socket
)

target_link_libraries(eos_hardware PRIVATE
  eos_backend_demo_arm eos_backend_demo_base eos_backend_demo_cam
  eos_backend_demo_gimbal eos_backend_demo_mic eos_backend_demo_gripper
  eos_backend_demo_speaker)


if(ISLAND_BUILD_TESTS)
  add_subdirectory(tests)
endif()

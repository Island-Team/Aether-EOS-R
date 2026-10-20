# runtime/hardware — backend registry: binding parsing, adapter instantiation,
# startup admission, tick, and health aggregation.
add_library(eos_hardware
  admission.cpp
  arm.cpp
  base.cpp
  binding.cpp
  camera.cpp
  common.cpp
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

# Demo headers are included by repository-relative path through the
# eos_internal interface include; the backend libraries themselves stay
# private to the hardware layer.
target_link_libraries(eos_hardware
  PRIVATE eos_internal
          eos_backend_aplay
          eos_backend_alsa
          eos_backend_v4l2
          tomlplusplus::tomlplusplus
          eos_lib
)

if(EOS_BACKEND_DEMO)
  target_link_libraries(eos_hardware PRIVATE
    eos_backend_demo_arm eos_backend_demo_base eos_backend_demo_cam
    eos_backend_demo_gimbal eos_backend_demo_mic eos_backend_demo_gripper
    eos_backend_demo_speaker)
  target_compile_definitions(eos_hardware PRIVATE EOS_BACKEND_DEMO=1)
endif()

if(EOS_BACKEND_ROS2)
  target_link_libraries(eos_hardware PRIVATE eos_backend_ros2)
  target_compile_definitions(eos_hardware PRIVATE EOS_BACKEND_ROS2=1)
endif()


if(ISLAND_BUILD_TESTS)
  add_subdirectory(tests)
endif()

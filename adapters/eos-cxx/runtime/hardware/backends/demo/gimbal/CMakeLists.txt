# Demo gimbal backend — loopback pan/tilt stub behind the south-bound
# interface.
add_library(eos_backend_demo_gimbal
  demo_gimbal.cpp
)

target_include_directories(eos_backend_demo_gimbal
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(eos_backend_demo_gimbal
  PRIVATE eos_backend_demo_core eos_internal
)

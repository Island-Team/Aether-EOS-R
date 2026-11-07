# Demo camera backend — feed-only loopback device behind the south-bound
# interface.
add_library(eos_backend_demo_cam
  demo_cam.cpp
)

target_include_directories(eos_backend_demo_cam
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(eos_backend_demo_cam
  PRIVATE eos_backend_demo_core eos_internal
)

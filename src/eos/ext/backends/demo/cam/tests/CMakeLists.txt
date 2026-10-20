# Demo camera backend structure test — world-owned source data through the
# south-bound interface.
add_executable(demo_cam_test
  demo_cam_test.cpp
)

target_include_directories(demo_cam_test
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(demo_cam_test
  PRIVATE eos_backend_demo_cam eos_simulator_world eos_internal
)

add_test(NAME demo_cam_test COMMAND demo_cam_test)

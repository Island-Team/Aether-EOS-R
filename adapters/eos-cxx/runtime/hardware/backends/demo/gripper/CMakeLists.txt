# Demo gripper backend — loopback jaw stub behind the south-bound interface.
add_library(eos_backend_demo_gripper
  demo_gripper.cpp
)

target_include_directories(eos_backend_demo_gripper
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(eos_backend_demo_gripper
  PRIVATE eos_backend_demo_core eos_internal
)

# Demo arm backend — loopback manipulator stub behind the south-bound
# interface.
add_library(eos_backend_demo_arm
  demo_arm.cpp
)

target_include_directories(eos_backend_demo_arm
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(eos_backend_demo_arm
  PRIVATE eos_backend_demo_core eos_internal
)

# Demo base backend — loopback planar-drive stub behind the south-bound
# interface.
add_library(eos_backend_demo_base
  demo_base.cpp
)

target_include_directories(eos_backend_demo_base
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(eos_backend_demo_base
  PRIVATE eos_backend_demo_core eos_internal
)

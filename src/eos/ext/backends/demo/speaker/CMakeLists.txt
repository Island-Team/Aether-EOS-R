# Demo speaker backend — loopback audio stub behind the south-bound
# interface.
add_library(eos_backend_demo_speaker
  demo_speaker.cpp
)

target_include_directories(eos_backend_demo_speaker
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(eos_backend_demo_speaker
  PRIVATE eos_backend_demo_core eos_internal
)

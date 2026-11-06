# Demo speaker backend structure test — stub data through the south-bound
# interface.
add_executable(demo_speaker_test
  demo_speaker_test.cpp
)

target_include_directories(demo_speaker_test
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(demo_speaker_test
  PRIVATE eos_backend_demo_speaker eos_internal
)

add_test(NAME demo_speaker_test COMMAND demo_speaker_test)

# Demo microphone backend structure test — stub data through the
# south-bound interface.
add_executable(demo_mic_test
  demo_mic_test.cpp
)

target_include_directories(demo_mic_test
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(demo_mic_test
  PRIVATE eos_backend_demo_mic eos_internal
)

add_test(NAME demo_mic_test COMMAND demo_mic_test)

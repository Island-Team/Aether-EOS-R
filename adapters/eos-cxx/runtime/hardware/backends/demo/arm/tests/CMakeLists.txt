# Demo arm backend structure test — stub data through the south-bound
# interface.
add_executable(demo_arm_test
  demo_arm_test.cpp
)

target_include_directories(demo_arm_test
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(demo_arm_test
  PRIVATE eos_backend_demo_arm eos_internal
)

add_test(NAME demo_arm_test COMMAND demo_arm_test)

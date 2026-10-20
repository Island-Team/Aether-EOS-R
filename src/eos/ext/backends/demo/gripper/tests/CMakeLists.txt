# Demo gripper backend structure test — stub data through the south-bound
# interface.
add_executable(demo_gripper_test
  demo_gripper_test.cpp
)

target_include_directories(demo_gripper_test
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(demo_gripper_test
  PRIVATE eos_backend_demo_gripper eos_internal
)

add_test(NAME demo_gripper_test COMMAND demo_gripper_test)

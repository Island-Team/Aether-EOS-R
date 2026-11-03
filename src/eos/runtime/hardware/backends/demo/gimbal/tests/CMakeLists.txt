# Demo gimbal backend structure test — stub data through the south-bound
# interface.
add_executable(demo_gimbal_test
  demo_gimbal_test.cpp
)

target_include_directories(demo_gimbal_test
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(demo_gimbal_test
  PRIVATE eos_backend_demo_gimbal eos_internal
)

add_test(NAME demo_gimbal_test COMMAND demo_gimbal_test)

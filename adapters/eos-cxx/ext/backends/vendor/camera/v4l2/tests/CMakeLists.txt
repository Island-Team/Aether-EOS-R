find_package(GTest REQUIRED)

add_executable(v4l2_cam_test
  v4l2_cam_test.cpp
)

target_include_directories(v4l2_cam_test
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(v4l2_cam_test
  PRIVATE eos_internal eos_backend_v4l2 GTest::gtest_main
)

add_test(NAME v4l2_cam_test COMMAND v4l2_cam_test)
set_tests_properties(v4l2_cam_test PROPERTIES TIMEOUT 30)

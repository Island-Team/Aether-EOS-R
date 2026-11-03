find_package(GTest REQUIRED)

add_executable(vendor_backend_tests
  vendor_base_test.cpp
  vendor_camera_test.cpp
)

target_link_libraries(vendor_backend_tests
  PRIVATE eos_backend_vendor eos_internal GTest::gtest GTest::gtest_main
)

add_test(NAME vendor_backend_tests COMMAND vendor_backend_tests)

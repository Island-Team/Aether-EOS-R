find_package(GTest REQUIRED)

add_executable(eos_north_http_audit_tests audit_test.cpp)
target_link_libraries(eos_north_http_audit_tests
  PRIVATE eos_north_http_audit GTest::gtest GTest::gtest_main
)
include(GoogleTest)
gtest_discover_tests(eos_north_http_audit_tests)

find_package(GTest REQUIRED)

add_executable(backend_config_tests config_test.cpp)
target_link_libraries(backend_config_tests PRIVATE eos_hardware eos_internal GTest::gtest GTest::gtest_main)
add_test(NAME backend_config_tests COMMAND backend_config_tests)

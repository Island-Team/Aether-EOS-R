find_package(GTest REQUIRED)

add_executable(eos_kernel_tests kernel_test.cpp)
target_link_libraries(eos_kernel_tests PRIVATE eos_kernel eos_internal GTest::gtest GTest::gtest_main)
target_compile_definitions(eos_kernel_tests PRIVATE EOS_CONTRACTS_ROOT="${EOS_CONTRACTS_DIR}")
add_test(NAME eos_kernel_tests COMMAND eos_kernel_tests)

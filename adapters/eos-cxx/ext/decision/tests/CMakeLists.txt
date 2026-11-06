find_package(GTest REQUIRED)

add_executable(eos_decision_tests runner_test.cpp)
target_link_libraries(eos_decision_tests
  PRIVATE eos_decision GTest::gtest GTest::gtest_main
)

add_test(NAME eos_decision_tests COMMAND eos_decision_tests)

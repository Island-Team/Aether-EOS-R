find_package(GTest REQUIRED)

add_executable(eos_lib_tests
  cancellation_test.cpp
  clock_test.cpp
  executor_test.cpp
  paths_test.cpp
  slot_test.cpp
  log_test.cpp
  timer_test.cpp
  uuid_test.cpp
)

target_link_libraries(eos_lib_tests
  PRIVATE eos_lib GTest::gtest GTest::gtest_main
)

add_test(NAME eos_lib_tests COMMAND eos_lib_tests)

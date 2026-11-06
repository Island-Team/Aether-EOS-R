find_package(GTest REQUIRED)
add_executable(eos_actionlist_tests journal_test.cpp manager_test.cpp)
target_link_libraries(eos_actionlist_tests PRIVATE eos_actionlist GTest::gtest GTest::gtest_main)
add_test(NAME eos_actionlist_tests COMMAND eos_actionlist_tests)

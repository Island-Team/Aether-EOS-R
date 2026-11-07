find_package(GTest REQUIRED)
add_executable(eos_skill_manager_tests journald_test.cpp manager_test.cpp)
target_link_libraries(eos_skill_manager_tests PRIVATE eos_skills GTest::gtest GTest::gtest_main)
add_test(NAME eos_skill_manager_tests COMMAND eos_skill_manager_tests)

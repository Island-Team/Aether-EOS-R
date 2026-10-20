find_package(GTest REQUIRED)
add_executable(eos_skills_core_tests admission_test.cpp)
target_link_libraries(eos_skills_core_tests PRIVATE eos_skills_core GTest::gtest GTest::gtest_main)
add_test(NAME eos_skills_core_tests COMMAND eos_skills_core_tests)

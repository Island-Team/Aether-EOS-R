find_package(GTest REQUIRED)
add_executable(eos_skills_builtin_tests
  execution_correctness_test.cpp
  primitive_lifecycle_test.cpp
  registry_test.cpp
  ${ISLAND_SOURCE_DIR}/share/skills/native/tests/assist_test.cpp
  ${ISLAND_SOURCE_DIR}/share/skills/native/head_find_trash/tests/head_find_trash_test.cpp
  ${ISLAND_SOURCE_DIR}/share/skills/native/head_find_trash_navigation_resume/tests/head_find_trash_navigation_resume_test.cpp
  ${ISLAND_SOURCE_DIR}/share/skills/native/observe_human/observe_human_test.cpp
  ${ISLAND_SOURCE_DIR}/share/skills/native/owner_interaction/owner_interaction_test.cpp
  ${ISLAND_SOURCE_DIR}/share/skills/native/pick_trash/tests/pick_trash_test.cpp
)
target_compile_features(eos_skills_builtin_tests PRIVATE cxx_std_20)
target_link_libraries(eos_skills_builtin_tests
  PRIVATE eos_skills_builtin island_native_skills GTest::gtest GTest::gtest_main
)
add_test(NAME eos_skills_builtin_tests COMMAND eos_skills_builtin_tests)

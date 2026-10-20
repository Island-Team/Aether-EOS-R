find_package(GTest REQUIRED)

add_executable(eos_workflow_registry_tests registry_test.cpp ../registry.cpp)
target_include_directories(eos_workflow_registry_tests PRIVATE "${ISLAND_SOURCE_DIR}")
target_compile_definitions(eos_workflow_registry_tests PRIVATE
  EOS_WORKFLOW_TEMPLATES_ROOT="${EOS_CONTRACTS_DIR}/workflows"
)
target_link_libraries(eos_workflow_registry_tests
  PRIVATE island_runtime_proto eos_lib GTest::gtest GTest::gtest_main
)
add_test(NAME eos_workflow_registry_tests COMMAND eos_workflow_registry_tests)

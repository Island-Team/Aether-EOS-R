# Gateway unit tests — the transport-neutral service core and adapters.
find_package(GTest REQUIRED)

add_executable(eos_northbound_tests
  duration_test.cpp
  service_test.cpp
  server_integration_test.cpp
  skill_codec_test.cpp
  subscribe_test.cpp
)

target_include_directories(eos_northbound_tests
  PRIVATE "${ISLAND_SOURCE_DIR}"
)

target_compile_definitions(eos_northbound_tests PRIVATE
  EOS_CONTRACTS_ROOT="${EOS_CONTRACTS_DIR}"
  EOS_SOURCE_ETC="${ISLAND_SOURCE_DIR}/etc/eos"
  EOS_TEST_SKILL_DIR="$<TARGET_FILE_DIR:eos_test_skill>"
)

target_link_libraries(eos_northbound_tests
  PRIVATE eos_northbound eos_internal eos_body eos_perception_history eos_resource_catalog
          eos_dispatch eos_hardware eos_lib eos_models
          eos_skills_native eos_simulator_service eos_simulator_ipc_client
          GTest::gtest GTest::gtest_main
)
add_dependencies(eos_northbound_tests eos_test_skill)

add_test(NAME eos_northbound_tests COMMAND eos_northbound_tests)
set_tests_properties(eos_northbound_tests PROPERTIES TIMEOUT 300)

find_package(GTest REQUIRED)

add_executable(eos_init_tests
  adapter_config_test.cpp
  advanced_models_test.cpp
  config_test.cpp
  inference_test.cpp
  rotated_frame_test.cpp
  schemas_test.cpp
)
target_compile_definitions(eos_init_tests PRIVATE
  EOS_CONTRACTS_ROOT="${EOS_CONTRACTS_DIR}"
  EOS_SOURCE_ETC="${ISLAND_SOURCE_DIR}/etc/eos"
)
target_link_libraries(eos_init_tests
  PRIVATE eos_init eos_init_schemas eos_init_config eos_hardware eos_models
          GTest::gtest GTest::gtest_main
)
add_test(NAME eos_init_tests COMMAND eos_init_tests)

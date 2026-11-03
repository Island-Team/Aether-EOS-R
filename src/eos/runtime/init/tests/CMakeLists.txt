find_package(GTest REQUIRED)

add_executable(eos_init_tests
  advanced_models_test.cpp
  config_test.cpp
  inference_test.cpp
  rotated_frame_test.cpp
  schemas_test.cpp
  stack_manager_test.cpp
  vlm_image_asset_test.cpp
)
target_include_directories(eos_init_tests PRIVATE "${ISLAND_SOURCE_DIR}")
target_compile_definitions(eos_init_tests PRIVATE
  EOS_CONTRACTS_ROOT="${EOS_CONTRACTS_DIR}"
  EOS_SOURCE_ETC="${ISLAND_SOURCE_DIR}/etc/eos"
)
target_link_libraries(eos_init_tests
  PRIVATE eos_init eos_init_schemas eos_init_config eos_init_stack_manager eos_hardware eos_models
          GTest::gtest GTest::gtest_main
)
add_test(NAME eos_init_tests COMMAND eos_init_tests)

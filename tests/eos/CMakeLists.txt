# Cross-module integration tests — call-chain tests with stub data.
add_executable(perception_chain_test
  integration/perception_chain_test.cpp
)

target_link_libraries(perception_chain_test
  PRIVATE eos_internal eos_lib eos_hardware eos_body eos_perception_history eos_resource_catalog
)

target_compile_definitions(perception_chain_test PRIVATE
  EOS_SOURCE_ETC="${ISLAND_SOURCE_DIR}/etc/eos"
)

add_test(NAME perception_chain_test COMMAND perception_chain_test)

add_test(
  NAME eos_config_interface_test
  COMMAND "${Python3_EXECUTABLE}" "${ISLAND_SOURCE_DIR}/tests/eos/test_eos_config_interface.py"
          "$<TARGET_FILE:eos-config>"
)

add_test(
  NAME island_package_client_test
  COMMAND "${Python3_EXECUTABLE}" "${ISLAND_SOURCE_DIR}/tests/eos/test_island_pkg.py"
)
add_test(
  NAME eos_build_config_test
  COMMAND "${Python3_EXECUTABLE}" "${ISLAND_SOURCE_DIR}/tests/eos/test_eos_build_config.py"
)
add_test(
  NAME island_launch_manager_test
  COMMAND "${Python3_EXECUTABLE}" "${ISLAND_SOURCE_DIR}/tests/eos/test_island_launch.py"
          "${ISLAND_SOURCE_DIR}"
)

add_test(
  NAME simulator_launch_selection_test
  COMMAND "${Python3_EXECUTABLE}"
          "${ISLAND_SOURCE_DIR}/tests/eos/test_simulator_launch_selection.py"
          "${ISLAND_SOURCE_DIR}"
)

add_executable(demo_arm_chain_test
  integration/demo_arm_chain_test.cpp
)

target_compile_definitions(demo_arm_chain_test PRIVATE
  EOS_CONTRACTS_ROOT="${EOS_CONTRACTS_DIR}"
  EOS_SOURCE_ETC="${ISLAND_SOURCE_DIR}/etc/eos"
)

target_link_libraries(demo_arm_chain_test
  PRIVATE eos_internal eos_lib eos_hardware eos_dispatch eos_body
)

eos_add_simulator_test(demo_arm_chain_test demo_arm_chain_test)

add_test(
  NAME contract_interfaces_script
  COMMAND "${Python3_EXECUTABLE}" "${ISLAND_SOURCE_DIR}/tests/eos/test_contract_interfaces.py"
          "${EOS_CONTRACTS_DIR}"
)
add_test(
  NAME contract_manifest_check
  COMMAND "${Python3_EXECUTABLE}" "${ISLAND_SOURCE_DIR}/scripts/contract_manifest.py"
          --root "${EOS_CONTRACTS_DIR}/schemas/eos"
          --output "${EOS_CONTRACTS_DIR}/schemas/eos/contracts.manifest.json"
          --check
)
add_test(
  NAME generated_contracts_check
  COMMAND "${Python3_EXECUTABLE}" "${ISLAND_SOURCE_DIR}/scripts/generate_eos_contracts.py"
          --proto-root "${ISLAND_SOURCE_DIR}/share/eos/contracts"
          --output "${EOS_CONTRACTS_DIR}/schemas/eos" --check
)
if(EOS_NORTH_HTTP)
  add_test(
    NAME generated_openapi_check
    COMMAND "${Python3_EXECUTABLE}" "${ISLAND_SOURCE_DIR}/scripts/generate_north_http_openapi.py"
            --proto-root "${ISLAND_SOURCE_DIR}/share/eos/contracts"
            --runtime-root "${EOS_CONTRACTS_DIR}/schemas/eos"
            --output "${EOS_NORTH_HTTP_OPENAPI_FILE}" --check)
  add_test(NAME openapi_contracts
    COMMAND "${Python3_EXECUTABLE}" "${ISLAND_SOURCE_DIR}/tests/eos/test_north_http_openapi.py")
endif()
add_test(NAME generated_contracts
  COMMAND "${Python3_EXECUTABLE}" "${ISLAND_SOURCE_DIR}/tests/eos/test_generate_eos_contracts.py")
add_test(
  NAME contract_manifest_script
  COMMAND "${Python3_EXECUTABLE}" "${ISLAND_SOURCE_DIR}/tests/eos/test_contract_manifest.py"
)
add_test(
  NAME doc_link_checker_test
  COMMAND "${Python3_EXECUTABLE}" "${ISLAND_SOURCE_DIR}/tests/eos/test_doc_links.py"
)
add_test(
  NAME doc_links_check
  COMMAND "${Python3_EXECUTABLE}" "${ISLAND_SOURCE_DIR}/scripts/check_doc_links.py" --all
)
add_test(
  NAME installed_wrapper_smoke
  COMMAND "${Python3_EXECUTABLE}" "${ISLAND_SOURCE_DIR}/tests/eos/test_installed_wrappers.py"
          "${CMAKE_COMMAND}" "${CMAKE_BINARY_DIR}" "${EOS_NORTH_HTTP}"
)

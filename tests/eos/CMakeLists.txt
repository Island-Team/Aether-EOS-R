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
          --root "${EOS_CONTRACTS_DIR}/ext/eos"
          --output "${EOS_CONTRACTS_DIR}/ext/eos/contracts.manifest.json"
          --check
)
add_test(
  NAME generated_contracts_check
  COMMAND "${Python3_EXECUTABLE}" "${ISLAND_SOURCE_DIR}/scripts/generate_eos_contracts.py"
          --proto-root "${ISLAND_SOURCE_DIR}/share/contracts"
          --output "${EOS_CONTRACTS_DIR}/ext/eos" --check
)
if(EOS_NORTH_HTTP)
  add_test(
    NAME generated_openapi_check
    COMMAND "${Python3_EXECUTABLE}" "${ISLAND_SOURCE_DIR}/scripts/generate_north_http_openapi.py"
            --proto-root "${ISLAND_SOURCE_DIR}/share/contracts"
            --runtime-root "${EOS_CONTRACTS_DIR}/ext/eos"
            --output "${EOS_CONTRACTS_DIR}/ext/north_http/openapi.json" --check)
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
  NAME installed_wrapper_smoke
  COMMAND "${Python3_EXECUTABLE}" "${ISLAND_SOURCE_DIR}/tests/eos/test_installed_wrappers.py"
          "${CMAKE_COMMAND}" "${CMAKE_BINARY_DIR}" "${EOS_NORTH_HTTP}"
)
find_program(EOS_NM_EXECUTABLE NAMES nm REQUIRED)
# nlohmann_json reaches EOS either as a system package or as a FetchContent
# clone, so hand the test whichever applies instead of letting it assume a
# build-directory layout. "-" marks the unset case.
if(EOS_NLOHMANN_JSON_DIR)
  set(EOS_NLOHMANN_JSON_DIR_ARG "${EOS_NLOHMANN_JSON_DIR}")
else()
  set(EOS_NLOHMANN_JSON_DIR_ARG "-")
endif()
if(EOS_NLOHMANN_JSON_SOURCE_DIR)
  set(EOS_NLOHMANN_JSON_SOURCE_DIR_ARG "${EOS_NLOHMANN_JSON_SOURCE_DIR}")
else()
  set(EOS_NLOHMANN_JSON_SOURCE_DIR_ARG "-")
endif()
add_test(
  NAME installed_skill_sdk
  COMMAND "${Python3_EXECUTABLE}" "${ISLAND_SOURCE_DIR}/tests/eos/test_skill_package.py"
          "${CMAKE_COMMAND}" "${EOS_NM_EXECUTABLE}" "${ISLAND_SOURCE_DIR}"
          "${CMAKE_BINARY_DIR}"
          "${EOS_NLOHMANN_JSON_DIR_ARG}" "${EOS_NLOHMANN_JSON_SOURCE_DIR_ARG}"
)
set_tests_properties(installed_skill_sdk PROPERTIES TIMEOUT 120)

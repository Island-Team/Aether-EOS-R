find_package(GTest REQUIRED)

add_executable(eos_skill_sdk_tests sdk_test.cpp)
target_link_libraries(eos_skill_sdk_tests
  PRIVATE eos_skill_cpp GTest::gtest GTest::gtest_main
)
add_test(NAME eos_skill_sdk_tests COMMAND eos_skill_sdk_tests)

add_library(eos_test_skill MODULE
  fixture.cpp
)

target_link_libraries(eos_test_skill PRIVATE eos_skill_api)
set_target_properties(eos_test_skill PROPERTIES
  PREFIX ""
  OUTPUT_NAME "fixture"
  SUFFIX ".skill.so"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plugins"
)

add_library(eos_test_skill_v2 MODULE fixture_v2.cpp)
target_link_libraries(eos_test_skill_v2 PRIVATE eos_skill_api)
set_target_properties(eos_test_skill_v2 PROPERTIES
  PREFIX ""
  OUTPUT_NAME "fixture-v2"
  SUFFIX ".skill.so"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plugins-v2"
)

add_library(eos_old_layout_skill_v2 MODULE fixture_v2_old_layout.cpp)
target_link_libraries(eos_old_layout_skill_v2 PRIVATE eos_skill_api)
set_target_properties(eos_old_layout_skill_v2 PROPERTIES
  PREFIX ""
  OUTPUT_NAME "fixture-v2-old-layout"
  SUFFIX ".skill.so"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plugins-v2-old-layout"
)

add_library(eos_bad_abi_skill MODULE
  bad_abi.cpp
)
target_link_libraries(eos_bad_abi_skill PRIVATE eos_skill_api)
set_target_properties(eos_bad_abi_skill PROPERTIES
  PREFIX ""
  OUTPUT_NAME "bad-abi"
  SUFFIX ".skill.so"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bad-abi"
)

add_library(eos_bad_primitive_skill MODULE
  bad_primitive.cpp
)

add_library(eos_bad_descriptor_stride_skill MODULE bad_descriptor_stride.cpp)
target_link_libraries(eos_bad_descriptor_stride_skill PRIVATE eos_skill_api)
set_target_properties(eos_bad_descriptor_stride_skill PROPERTIES
  PREFIX ""
  OUTPUT_NAME "bad-descriptor-stride"
  SUFFIX ".skill.so"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bad-descriptor-stride"
)

add_library(eos_bad_descriptor_size_skill MODULE bad_descriptor_size.cpp)
target_link_libraries(eos_bad_descriptor_size_skill PRIVATE eos_skill_api)
set_target_properties(eos_bad_descriptor_size_skill PROPERTIES
  PREFIX ""
  OUTPUT_NAME "bad-descriptor-size"
  SUFFIX ".skill.so"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bad-descriptor-size"
)
target_link_libraries(eos_bad_primitive_skill PRIVATE eos_skill_api)
set_target_properties(eos_bad_primitive_skill PROPERTIES
  PREFIX ""
  OUTPUT_NAME "bad-primitive"
  SUFFIX ".skill.so"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bad-primitive"
)

add_library(eos_legacy_v1_skill MODULE
  legacy_v1.cpp
)
set_target_properties(eos_legacy_v1_skill PROPERTIES
  PREFIX ""
  OUTPUT_NAME "legacy-v1"
  SUFFIX ".skill.so"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/legacy-v1"
)

add_executable(eos_native_skills_tests
  runtime_test.cpp
)

target_compile_definitions(eos_native_skills_tests PRIVATE
  EOS_CONTRACTS_ROOT="${EOS_CONTRACTS_DIR}"
  EOS_SOURCE_ETC="${ISLAND_SOURCE_DIR}/etc/eos"
  EOS_TEST_SKILL_DIR="$<TARGET_FILE_DIR:eos_test_skill>"
  EOS_TEST_SKILL_V2_DIR="$<TARGET_FILE_DIR:eos_test_skill_v2>"
  EOS_OLD_LAYOUT_SKILL_V2_DIR="$<TARGET_FILE_DIR:eos_old_layout_skill_v2>"
  EOS_BAD_ABI_SKILL_DIR="$<TARGET_FILE_DIR:eos_bad_abi_skill>"
  EOS_BAD_PRIMITIVE_SKILL_DIR="$<TARGET_FILE_DIR:eos_bad_primitive_skill>"
  EOS_BAD_DESCRIPTOR_STRIDE_SKILL_DIR="$<TARGET_FILE_DIR:eos_bad_descriptor_stride_skill>"
  EOS_BAD_DESCRIPTOR_SIZE_SKILL_DIR="$<TARGET_FILE_DIR:eos_bad_descriptor_size_skill>"
  EOS_LEGACY_V1_SKILL_DIR="$<TARGET_FILE_DIR:eos_legacy_v1_skill>"
)
target_link_libraries(eos_native_skills_tests
  PRIVATE eos_skills eos_skills_native eos_actionlist eos_simulator_ipc_client
          eos_simulator_ipc_server GTest::gtest GTest::gtest_main
)
add_dependencies(eos_native_skills_tests eos_test_skill eos_test_skill_v2 eos_old_layout_skill_v2 eos_bad_abi_skill
                 eos_bad_primitive_skill eos_bad_descriptor_stride_skill
                 eos_bad_descriptor_size_skill eos_legacy_v1_skill)

add_test(NAME eos_native_skills_tests COMMAND eos_native_skills_tests)
set_tests_properties(eos_native_skills_tests PROPERTIES TIMEOUT 300)

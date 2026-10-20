# State wire-projection tests — unit tests with demo feeds: every builder's
# output validates against the repo's real share/ schemas (a local
# validator helper, not init's SchemaStore — see reader_test.cpp).
find_package(GTest REQUIRED)

add_executable(eos_body_tests
  boundary_test.cpp
  detection_overlay_test.cpp
)

target_include_directories(eos_body_tests
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# Read-only committed inputs: the contract tree the schema oracle compiles
# and the etc/ bindings the fixtures admit.
target_compile_definitions(eos_body_tests PRIVATE
  EOS_SOURCE_ETC="${ISLAND_SOURCE_DIR}/etc/eos"
)

target_link_libraries(eos_body_tests
  PRIVATE eos_body eos_hardware eos_internal
          GTest::gtest GTest::gtest_main
)

eos_add_simulator_test(eos_body_tests eos_body_tests)

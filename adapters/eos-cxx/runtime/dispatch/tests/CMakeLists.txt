# Dispatch unit tests — primitive happy paths and distinct rejection
# reasons, base safety leases, and ack validation against the real
# ack.v1.json (via init's shared SchemaStore).
find_package(GTest REQUIRED)

add_executable(eos_dispatch_tests
  ack_test.cpp
  primitives_test.cpp
)

target_include_directories(eos_dispatch_tests
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# The committed etc/ tree is read-only input (committed bindings), and the
# contracts tree backs the ack oracle.
target_compile_definitions(eos_dispatch_tests PRIVATE
  EOS_SOURCE_ETC="${ISLAND_SOURCE_DIR}/etc/eos"
  EOS_CONTRACTS_ROOT="${EOS_CONTRACTS_DIR}"
)

target_link_libraries(eos_dispatch_tests
  PRIVATE eos_actionlist eos_dispatch eos_internal
          eos_northbound
          eos_backend_demo_arm
          eos_backend_demo_base
          eos_backend_demo_gimbal
          eos_backend_demo_gripper
          eos_backend_demo_speaker
          GTest::gtest GTest::gtest_main
)

eos_add_simulator_test(eos_dispatch_tests eos_dispatch_tests)
set_property(TEST eos_dispatch_tests APPEND PROPERTY ENVIRONMENT "EOS_TEST_ALSA_NULL=1")

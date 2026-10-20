# Bridge registry tests — unit tests with the committed loopback bindings
# plus inline malformed fixtures.
find_package(GTest REQUIRED)

add_executable(hardware_tests
  binding_test.cpp
  mode_test.cpp
  registry_test.cpp
  telemetry_hub_test.cpp
)

target_include_directories(hardware_tests
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(hardware_tests
  PRIVATE eos_hardware eos_internal GTest::gtest GTest::gtest_main
)

# The committed etc/ tree is read-only input: tests parse eos.example.toml
# and backends/demo.toml so config regressions fail here, not at launch.
target_compile_definitions(hardware_tests PRIVATE
  EOS_SOURCE_ETC="${ISLAND_SOURCE_DIR}/etc/eos"
)
if(EOS_BACKEND_ROS2)
  target_compile_definitions(hardware_tests PRIVATE EOS_BACKEND_ROS2=1)
endif()

eos_add_simulator_test(hardware_tests hardware_tests)

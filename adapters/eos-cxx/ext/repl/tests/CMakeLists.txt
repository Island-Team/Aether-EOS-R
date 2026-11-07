# REPL tests — pure parser/renderer coverage plus a loopback adapter backed by
# a fake gRPC EOS service.
find_package(GTest REQUIRED)

add_executable(eos_repl_tests
  auto_base_test.cpp
  catalog_test.cpp
  command_test.cpp
  completion_test.cpp
  format_test.cpp
  history_test.cpp
  json_input_test.cpp
  options_test.cpp
  prompt_test.cpp
  session_test.cpp
  ../session.cpp
)

target_include_directories(eos_repl_tests
  PRIVATE "${ISLAND_SOURCE_DIR}"
)

target_link_libraries(eos_repl_tests
  PRIVATE eos_repl_core eos_north_http_client eos_north_http_config eos_north_http_server
          GTest::gtest GTest::gtest_main
          Boost::boost Threads::Threads
)
target_compile_definitions(eos_repl_tests PRIVATE EOS_REPL_CONTRACTS_ROOT="${EOS_CONTRACTS_DIR}")

add_test(NAME eos_repl_tests COMMAND eos_repl_tests)

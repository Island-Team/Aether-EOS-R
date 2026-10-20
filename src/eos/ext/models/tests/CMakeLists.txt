find_package(GTest REQUIRED)

add_executable(eos_model_adapters_tests
  external_adapter_test.cpp
  stream_adapter_test.cpp
)

target_link_libraries(eos_model_adapters_tests
  PRIVATE eos_model_adapters Boost::boost GTest::gtest GTest::gtest_main
)

add_test(NAME eos_model_adapters_tests COMMAND eos_model_adapters_tests)

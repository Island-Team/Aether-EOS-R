find_package(GTest REQUIRED)

add_executable(eos_models_tests
  camera_object_test.cpp
  camera_pose_test.cpp
  runtime_test.cpp
  grpc_test.cpp
  microphone_vad_test.cpp
)

target_link_libraries(eos_models_tests
  PRIVATE eos_models eos_model_provider_proto Boost::boost GTest::gtest GTest::gtest_main
)
target_compile_definitions(eos_models_tests PRIVATE EOS_SOURCE_ETC="${ISLAND_SOURCE_DIR}/etc/eos")

add_test(NAME eos_models_tests COMMAND eos_models_tests)

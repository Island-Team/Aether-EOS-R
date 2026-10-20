add_library(eos_models
  camera_object.cpp
  camera_pose.cpp
  microphone_vad.cpp
  runtime.cpp
  vision_stream.cpp
  grpc_client.cpp
)

set(EOS_MODEL_PROTO "${ISLAND_SOURCE_DIR}/share/contracts/eos/external/models/model_provider.proto")
set(EOS_MODEL_PROTO_GEN "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${EOS_MODEL_PROTO_GEN}")
if(TARGET protobuf::protoc)
  set(EOS_MODEL_PROTOC "$<TARGET_FILE:protobuf::protoc>")
else()
  set(EOS_MODEL_PROTOC "${Protobuf_PROTOC_EXECUTABLE}")
endif()
if(TARGET gRPC::grpc_cpp_plugin)
  set(EOS_MODEL_GRPC_PLUGIN "$<TARGET_FILE:gRPC::grpc_cpp_plugin>")
elseif(TARGET grpc_cpp_plugin)
  set(EOS_MODEL_GRPC_PLUGIN "$<TARGET_FILE:grpc_cpp_plugin>")
else()
  find_program(EOS_MODEL_GRPC_PLUGIN grpc_cpp_plugin REQUIRED)
endif()
set(EOS_MODEL_PROTO_FILES
  "${EOS_MODEL_PROTO_GEN}/model_provider.pb.cc"
  "${EOS_MODEL_PROTO_GEN}/model_provider.pb.h"
  "${EOS_MODEL_PROTO_GEN}/model_provider.grpc.pb.cc"
  "${EOS_MODEL_PROTO_GEN}/model_provider.grpc.pb.h")
add_custom_command(OUTPUT ${EOS_MODEL_PROTO_FILES}
  COMMAND "${EOS_MODEL_PROTOC}" "--proto_path=${ISLAND_SOURCE_DIR}/share/contracts/eos/external/models"
          "--cpp_out=${EOS_MODEL_PROTO_GEN}" "${EOS_MODEL_PROTO}"
  COMMAND "${EOS_MODEL_PROTOC}" "--proto_path=${ISLAND_SOURCE_DIR}/share/contracts/eos/external/models"
          "--grpc_out=${EOS_MODEL_PROTO_GEN}"
          "--plugin=protoc-gen-grpc=${EOS_MODEL_GRPC_PLUGIN}" "${EOS_MODEL_PROTO}"
  DEPENDS "${EOS_MODEL_PROTO}" VERBATIM)
add_library(eos_model_provider_proto ${EOS_MODEL_PROTO_FILES})
target_include_directories(eos_model_provider_proto PUBLIC "${EOS_MODEL_PROTO_GEN}")
target_link_libraries(eos_model_provider_proto PUBLIC protobuf::libprotobuf gRPC::grpc++)

find_package(PNG REQUIRED)

target_link_libraries(eos_models
  PUBLIC eos_internal nlohmann_json::nlohmann_json
  PRIVATE eos_lib Boost::boost tomlplusplus::tomlplusplus nlohmann_json_schema_validator PNG::PNG eos_model_provider_proto
)
target_include_directories(eos_models PRIVATE "${EOS_MODEL_PROTO_GEN}")

include(GNUInstallDirs)
set(EOS_MODEL_SCHEMA_SOURCE_DIR "${ISLAND_SOURCE_DIR}/share/contracts/eos/external/models/schemas")
set(EOS_MODEL_SCHEMA_INSTALL_DIR "${CMAKE_INSTALL_FULL_DATADIR}/island/eos/models/schemas")
target_compile_definitions(eos_models PRIVATE
  EOS_MODEL_SCHEMA_SOURCE_DIR="${EOS_MODEL_SCHEMA_SOURCE_DIR}"
  EOS_MODEL_SCHEMA_INSTALL_DIR="${EOS_MODEL_SCHEMA_INSTALL_DIR}"
)
install(DIRECTORY "${EOS_MODEL_SCHEMA_SOURCE_DIR}/"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/island/eos/models/schemas"
  FILES_MATCHING PATTERN "*.v1.json"
)

if(ISLAND_BUILD_TESTS)
  add_subdirectory(tests)
endif()

find_package(PNG REQUIRED)
find_program(EOS_GSTREAMER_LAUNCH gst-launch-1.0 REQUIRED)

add_library(eos_simulator_world world.cpp raster_map.cpp scene.cpp source_media.cpp
  system_camera_source.cpp monitor.cpp map_store.cpp)
target_include_directories(eos_simulator_world PUBLIC "${ISLAND_SOURCE_DIR}")
target_link_libraries(eos_simulator_world
  PRIVATE eos_lib
  PUBLIC eos_internal PNG::PNG nlohmann_json::nlohmann_json
)
target_compile_definitions(eos_simulator_world PRIVATE
  EOS_GSTREAMER_LAUNCH="${EOS_GSTREAMER_LAUNCH}"
)

set(EOS_SIMULATOR_PROTO_GEN "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(EOS_SIMULATOR_PROTO_DIR "${ISLAND_SOURCE_DIR}/share/contracts/eos/external/simulator")
set(EOS_SIMULATOR_PROTO "${EOS_SIMULATOR_PROTO_DIR}/simulator.proto")
file(MAKE_DIRECTORY "${EOS_SIMULATOR_PROTO_GEN}")
if(TARGET protobuf::protoc)
  set(EOS_SIMULATOR_PROTOC "$<TARGET_FILE:protobuf::protoc>")
else()
  set(EOS_SIMULATOR_PROTOC "${Protobuf_PROTOC_EXECUTABLE}")
endif()
if(TARGET gRPC::grpc_cpp_plugin)
  set(EOS_SIMULATOR_GRPC_PLUGIN "$<TARGET_FILE:gRPC::grpc_cpp_plugin>")
elseif(TARGET grpc_cpp_plugin)
  set(EOS_SIMULATOR_GRPC_PLUGIN "$<TARGET_FILE:grpc_cpp_plugin>")
else()
  find_program(EOS_SIMULATOR_GRPC_PLUGIN grpc_cpp_plugin REQUIRED)
endif()
set(EOS_SIMULATOR_PROTO_FILES
  "${EOS_SIMULATOR_PROTO_GEN}/simulator.pb.cc"
  "${EOS_SIMULATOR_PROTO_GEN}/simulator.pb.h"
  "${EOS_SIMULATOR_PROTO_GEN}/simulator.grpc.pb.cc"
  "${EOS_SIMULATOR_PROTO_GEN}/simulator.grpc.pb.h")
add_custom_command(OUTPUT ${EOS_SIMULATOR_PROTO_FILES}
  COMMAND "${EOS_SIMULATOR_PROTOC}" "--proto_path=${EOS_SIMULATOR_PROTO_DIR}"
          "--experimental_allow_proto3_optional"
          "--cpp_out=${EOS_SIMULATOR_PROTO_GEN}" "${EOS_SIMULATOR_PROTO}"
  COMMAND "${EOS_SIMULATOR_PROTOC}" "--proto_path=${EOS_SIMULATOR_PROTO_DIR}"
          "--experimental_allow_proto3_optional"
          "--grpc_out=${EOS_SIMULATOR_PROTO_GEN}"
          "--plugin=protoc-gen-grpc=${EOS_SIMULATOR_GRPC_PLUGIN}" "${EOS_SIMULATOR_PROTO}"
  DEPENDS "${EOS_SIMULATOR_PROTO}" VERBATIM)
add_library(eos_simulator_proto ${EOS_SIMULATOR_PROTO_FILES})
target_include_directories(eos_simulator_proto PUBLIC "${EOS_SIMULATOR_PROTO_GEN}")
target_link_libraries(eos_simulator_proto PUBLIC protobuf::libprotobuf gRPC::grpc++)

add_library(eos_simulator_ipc_client ipc/client.cpp)
target_include_directories(eos_simulator_ipc_client PUBLIC "${ISLAND_SOURCE_DIR}")
target_link_libraries(eos_simulator_ipc_client PUBLIC eos_simulator_proto)

add_library(eos_simulator_ipc_server ipc/server.cpp)
target_include_directories(eos_simulator_ipc_server PUBLIC "${ISLAND_SOURCE_DIR}")
target_link_libraries(eos_simulator_ipc_server PUBLIC eos_simulator_proto eos_simulator_world eos_lib)

add_library(eos_simulator_service config.cpp web_server.cpp)
target_include_directories(eos_simulator_service PUBLIC "${ISLAND_SOURCE_DIR}")
target_link_libraries(eos_simulator_service
  PUBLIC eos_simulator_world eos_lib nlohmann_json::nlohmann_json
  PUBLIC eos_simulator_ipc_server
  PRIVATE tomlplusplus::tomlplusplus
)

if(ISLAND_BUILD_TESTS)
  add_subdirectory(tests)
endif()

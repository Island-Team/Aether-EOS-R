set(ISLAND_RUNTIME_CONTRACT_ROOT "${ISLAND_SOURCE_DIR}/share/contracts")
set(EOS_GATEWAY_PROTO_GEN "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(EOS_GATEWAY_PROTO "${ISLAND_RUNTIME_CONTRACT_ROOT}/eos/gateway.proto")
set(EOS_SKILLS_PROTO "${ISLAND_RUNTIME_CONTRACT_ROOT}/eos/skills.proto")
set(EOS_WORKFLOW_PROTO "${ISLAND_RUNTIME_CONTRACT_ROOT}/eos/workflow.proto")
set(EOS_HTTP_PROTO "${ISLAND_RUNTIME_CONTRACT_ROOT}/google/api/http.proto")
set(EOS_ANNOTATIONS_PROTO "${ISLAND_RUNTIME_CONTRACT_ROOT}/google/api/annotations.proto")

# Every EOS contract, including validation metadata and payload messages, is
# compiled for reflection. Only the three service entry points produce gRPC stubs.
file(GLOB_RECURSE EOS_CONTRACT_PROTOS CONFIGURE_DEPENDS
  "${ISLAND_RUNTIME_CONTRACT_ROOT}/eos/*.proto")
# Private peer protocols have their own build targets and are not part of the
# public EOS reflection library.
list(FILTER EOS_CONTRACT_PROTOS EXCLUDE REGEX "/eos/external/(models|simulator)/")
list(SORT EOS_CONTRACT_PROTOS)

file(MAKE_DIRECTORY "${EOS_GATEWAY_PROTO_GEN}")

if(TARGET protobuf::protoc)
  set(EOS_PROTOC "$<TARGET_FILE:protobuf::protoc>")
elseif(Protobuf_PROTOC_EXECUTABLE)
  set(EOS_PROTOC "${Protobuf_PROTOC_EXECUTABLE}")
else()
  message(FATAL_ERROR "eos: protoc is required to generate the gateway interface")
endif()

if(TARGET gRPC::grpc_cpp_plugin)
  set(EOS_GRPC_CPP_PLUGIN "$<TARGET_FILE:gRPC::grpc_cpp_plugin>")
elseif(TARGET grpc_cpp_plugin)
  set(EOS_GRPC_CPP_PLUGIN "$<TARGET_FILE:grpc_cpp_plugin>")
else()
  find_program(EOS_GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)
endif()

set(EOS_PROTOBUF_INCLUDE_ARGS "-I${ISLAND_RUNTIME_CONTRACT_ROOT}")
if(Protobuf_INCLUDE_DIRS)
  foreach(include_dir IN LISTS Protobuf_INCLUDE_DIRS)
    list(APPEND EOS_PROTOBUF_INCLUDE_ARGS "-I${include_dir}")
  endforeach()
elseif(TARGET protobuf::libprotobuf)
  get_target_property(_protobuf_source protobuf::libprotobuf SOURCE_DIR)
  if(_protobuf_source)
    list(APPEND EOS_PROTOBUF_INCLUDE_ARGS "-I${_protobuf_source}/src")
  endif()
endif()
if(DEFINED grpc_SOURCE_DIR AND EXISTS "${grpc_SOURCE_DIR}/third_party/protobuf/src")
  list(APPEND EOS_PROTOBUF_INCLUDE_ARGS "-I${grpc_SOURCE_DIR}/third_party/protobuf/src")
endif()

set(EOS_GATEWAY_PROTO_SOURCES)
set(EOS_GATEWAY_PROTO_HEADERS)
foreach(proto IN ITEMS ${EOS_HTTP_PROTO} ${EOS_ANNOTATIONS_PROTO} ${EOS_CONTRACT_PROTOS})
  file(RELATIVE_PATH relative_proto "${ISLAND_RUNTIME_CONTRACT_ROOT}" "${proto}")
  string(REGEX REPLACE "\\.proto$" "" generated_stem "${relative_proto}")
  list(APPEND EOS_GATEWAY_PROTO_SOURCES "${EOS_GATEWAY_PROTO_GEN}/${generated_stem}.pb.cc")
  list(APPEND EOS_GATEWAY_PROTO_HEADERS "${EOS_GATEWAY_PROTO_GEN}/${generated_stem}.pb.h")
endforeach()
foreach(service IN ITEMS gateway skills workflow)
  list(APPEND EOS_GATEWAY_PROTO_SOURCES "${EOS_GATEWAY_PROTO_GEN}/eos/${service}.grpc.pb.cc")
  list(APPEND EOS_GATEWAY_PROTO_HEADERS "${EOS_GATEWAY_PROTO_GEN}/eos/${service}.grpc.pb.h")
endforeach()

add_custom_command(
  OUTPUT ${EOS_GATEWAY_PROTO_SOURCES} ${EOS_GATEWAY_PROTO_HEADERS}
  COMMAND "${EOS_PROTOC}" ${EOS_PROTOBUF_INCLUDE_ARGS}
          "--experimental_allow_proto3_optional"
          "--cpp_out=${EOS_GATEWAY_PROTO_GEN}"
          "${EOS_HTTP_PROTO}" "${EOS_ANNOTATIONS_PROTO}" ${EOS_CONTRACT_PROTOS}
  COMMAND "${EOS_PROTOC}" ${EOS_PROTOBUF_INCLUDE_ARGS}
          "--experimental_allow_proto3_optional"
          "--grpc_out=${EOS_GATEWAY_PROTO_GEN}"
          "--plugin=protoc-gen-grpc=${EOS_GRPC_CPP_PLUGIN}"
          "${EOS_GATEWAY_PROTO}" "${EOS_SKILLS_PROTO}" "${EOS_WORKFLOW_PROTO}"
  DEPENDS ${EOS_HTTP_PROTO} ${EOS_ANNOTATIONS_PROTO} ${EOS_CONTRACT_PROTOS}
  COMMENT "Generating EOS gRPC/ProtoJSON interface"
  VERBATIM
)

add_library(island_runtime_proto ${EOS_GATEWAY_PROTO_SOURCES} ${EOS_GATEWAY_PROTO_HEADERS})
target_include_directories(island_runtime_proto PUBLIC "${EOS_GATEWAY_PROTO_GEN}")
target_link_libraries(island_runtime_proto PUBLIC protobuf::libprotobuf gRPC::grpc++)

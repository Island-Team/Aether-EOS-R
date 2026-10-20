add_library(eos_northbound
  server.cpp
  service.cpp
  skill_codec.cpp
  skill_service.cpp
  skill_watch.cpp
  subscribe.cpp
)

add_subdirectory(workflow)

target_include_directories(eos_northbound
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(eos_northbound PRIVATE
  EOS_PROJECT_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(eos_northbound
  PUBLIC  island_runtime_proto nlohmann_json::nlohmann_json
  # The method layer routes through the projection and dispatcher by pointer
  # (NorthboundServices), so their interfaces are link-time needs of handler.cpp.
  PRIVATE eos_internal eos_lib eos_init_schemas
          gRPC::grpc++_reflection
          eos_body eos_perception_history eos_resource_catalog eos_dispatch eos_skills eos_actionlist eos_models eos_navigation_store
)

if(ISLAND_BUILD_TESTS)
  add_subdirectory(tests)
endif()

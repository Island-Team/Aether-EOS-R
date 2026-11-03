add_library(eos_init_schemas schemas.cpp)
add_dependencies(eos_init_schemas eos_contracts)
target_link_libraries(eos_init_schemas
  PUBLIC eos_internal nlohmann_json::nlohmann_json
  PRIVATE eos_lib nlohmann_json_schema_validator OpenSSL::Crypto
)

add_library(eos_init_config config.cpp)
target_link_libraries(eos_init_config
  PUBLIC eos_internal eos_lib eos_hardware eos_models
  PRIVATE tomlplusplus::tomlplusplus
)

add_library(eos_init_stack_manager stack_manager.cpp skill_grants.cpp)
target_link_libraries(eos_init_stack_manager
  PUBLIC eos_internal eos_hardware eos_skills_core eos_skills_worker_protocol nlohmann_json::nlohmann_json
  PRIVATE tomlplusplus::tomlplusplus OpenSSL::Crypto
)

find_package(JPEG REQUIRED)
find_package(PNG REQUIRED)
add_library(eos_init runtime.cpp rotated_frame.cpp vlm_image_asset.cpp)
target_link_libraries(eos_init
  PUBLIC eos_internal
  PRIVATE eos_init_schemas eos_init_config eos_init_stack_manager eos_northbound eos_lib eos_hardware eos_body
          eos_resource_catalog eos_perception_producers
          eos_dispatch eos_kernel eos_skills eos_skills_worker_provider
          eos_actionlist eos_models eos_model_adapters eos_navigation_store JPEG::JPEG PNG::PNG
)


if(ISLAND_BUILD_TESTS)
  add_subdirectory(tests)
endif()

add_executable(eosd
  eos-2607.cpp
)

target_link_libraries(eosd
  PRIVATE eos_init eos_lib
)
add_dependencies(eosd eos-skill-worker)

add_executable(eos-config eos-config.cpp)
target_link_libraries(eos-config PRIVATE eos_init_config nlohmann_json::nlohmann_json)
target_compile_definitions(eos-config PRIVATE EOS_PRODUCT_VERSION="${PROJECT_VERSION}")

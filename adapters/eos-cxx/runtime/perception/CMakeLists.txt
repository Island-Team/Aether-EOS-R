add_library(eos_perception_history product_history.cpp)
target_link_libraries(eos_perception_history PUBLIC eos_internal nlohmann_json::nlohmann_json)

add_library(eos_resource_catalog resource_catalog.cpp)
target_link_libraries(eos_resource_catalog
  PUBLIC eos_perception_history eos_perception_producers eos_internal nlohmann_json::nlohmann_json
  PRIVATE eos_body
)

add_library(eos_perception_producers producer_coordinator.cpp)
target_link_libraries(eos_perception_producers PUBLIC eos_internal)

if(ISLAND_BUILD_TESTS)
  add_subdirectory(tests)
endif()

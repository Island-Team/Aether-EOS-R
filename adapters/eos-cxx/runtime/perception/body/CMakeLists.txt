# runtime/perception/body — read-only north-bound body snapshots.
add_library(eos_body
  arm.cpp
  base.cpp
  camera.cpp
  common.cpp
  detection_overlay.cpp
  gimbal.cpp
  gripper.cpp
  inventory.cpp
  mic.cpp
  router.cpp
  semantic.cpp
  speaker.cpp
  vision.cpp
)

target_include_directories(eos_body
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(eos_body
  # reader.h returns nlohmann::json by value, so JSON is part of the API.
  PUBLIC  nlohmann_json::nlohmann_json
  PRIVATE eos_internal eos_lib eos_hardware eos_models eos_navigation_store
)

if(ISLAND_BUILD_TESTS)
  add_subdirectory(tests)
endif()

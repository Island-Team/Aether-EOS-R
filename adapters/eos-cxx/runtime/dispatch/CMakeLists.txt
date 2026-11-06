# runtime/dispatch — backend-aligned primitive dispatch: validated primitive
# calls in, one release out, contract-shaped acknowledgements back.
add_library(eos_dispatch
  ack.cpp
  arm.cpp
  base.cpp
  command.cpp
  common.cpp
  dispatcher.cpp
  face.cpp
  gimbal.cpp
  gripper.cpp
  safety.cpp
  speaker.cpp
  speaker_audio.cpp
)

target_include_directories(eos_dispatch
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(eos_dispatch
  PUBLIC  eos_hardware eos_init_schemas nlohmann_json::nlohmann_json
  PRIVATE eos_internal eos_lib eos_models
)

if(ISLAND_BUILD_TESTS)
  add_subdirectory(tests)
endif()

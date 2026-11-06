add_library(eos_model_adapters
  adapter_utils.cpp
  arcface_adapter.cpp
  audio8_speech_adapter.cpp
  canonical_adapter.cpp
  external_adapter.cpp
  gemma4_posture_adapter.cpp
  grasp_plan_adapter.cpp
  legacy_vision_adapter.cpp
  multipart_asr_adapter.cpp
  openai_chat_adapter.cpp
  openai_vlm_adapter.cpp
  sensevoice_vad_adapter.cpp
  yolo11_litter_adapter.cpp
  yolo11n_rtmpose_adapter.cpp
)

target_include_directories(eos_model_adapters
  PUBLIC "${ISLAND_SOURCE_DIR}"
)

target_link_libraries(eos_model_adapters
  PUBLIC eos_models nlohmann_json::nlohmann_json
  PRIVATE eos_lib
)

if(ISLAND_BUILD_TESTS)
  add_subdirectory(tests)
endif()

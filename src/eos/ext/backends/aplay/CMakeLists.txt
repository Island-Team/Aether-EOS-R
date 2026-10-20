# Native Linux speaker backend: synthesize through EOS and play through ALSA.
find_package(Threads REQUIRED)

add_library(eos_backend_aplay
  aplay_speaker.cpp
)

target_include_directories(eos_backend_aplay
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(eos_backend_aplay
  PRIVATE eos_internal eos_lib Threads::Threads
)

if(ISLAND_BUILD_TESTS)
  add_subdirectory(tests)
endif()

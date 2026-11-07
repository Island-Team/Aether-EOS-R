# Native Linux microphone backend: capture the host input through ALSA.
find_package(Threads REQUIRED)

add_library(eos_backend_alsa
  alsa_mic.cpp
)

target_include_directories(eos_backend_alsa
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(eos_backend_alsa
  PRIVATE eos_internal eos_lib Threads::Threads
)

if(ISLAND_BUILD_TESTS)
  add_subdirectory(tests)
endif()

# Native Linux camera backend: capture a host camera through Video4Linux2.
find_package(Threads REQUIRED)

add_library(eos_backend_v4l2
  v4l2_cam.cpp
)

target_include_directories(eos_backend_v4l2
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(eos_backend_v4l2
  PRIVATE eos_internal eos_lib Threads::Threads
)

if(ISLAND_BUILD_TESTS)
  add_subdirectory(tests)
endif()

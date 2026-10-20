add_library(eos_lib
  cancellation.cpp
  executor.cpp
  log.cpp
  paths.cpp
  timer.cpp
  uuid.cpp
)

target_include_directories(eos_lib
  PUBLIC "${ISLAND_SOURCE_DIR}"
)

find_package(spdlog REQUIRED)
find_package(Threads REQUIRED)

target_link_libraries(eos_lib
  PUBLIC spdlog::spdlog nlohmann_json::nlohmann_json Threads::Threads
)

target_compile_definitions(eos_lib PRIVATE
  EOS_PROJECT_VERSION="${PROJECT_VERSION}"
)

if(ISLAND_BUILD_TESTS)
  add_subdirectory(tests)
endif()

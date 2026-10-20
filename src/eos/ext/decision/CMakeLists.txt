# Backend-neutral external decision layer. This target intentionally depends on
# the generated northbound API and not on EOS runtime implementation targets.
find_package(Threads REQUIRED)

add_library(eos_decision
  client.cpp
  runner.cpp
)

target_include_directories(eos_decision
  PUBLIC "${ISLAND_SOURCE_DIR}"
)

target_link_libraries(eos_decision
  PUBLIC island_runtime_proto nlohmann_json::nlohmann_json Threads::Threads
)

if(ISLAND_BUILD_TESTS)
  add_subdirectory(tests)
endif()

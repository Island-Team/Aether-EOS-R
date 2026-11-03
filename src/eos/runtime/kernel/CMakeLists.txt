add_library(eos_kernel catalog.cpp broker.cpp)
target_link_libraries(eos_kernel PUBLIC eos_hardware eos_dispatch eos_skills_worker_protocol
                                        nlohmann_json::nlohmann_json
                                 PRIVATE eos_internal)

if(ISLAND_BUILD_TESTS)
  add_subdirectory(tests)
endif()

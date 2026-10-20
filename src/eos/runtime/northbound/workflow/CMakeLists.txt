target_sources(eos_northbound PRIVATE registry.cpp service.cpp)

if(ISLAND_BUILD_TESTS)
  add_subdirectory(tests)
endif()

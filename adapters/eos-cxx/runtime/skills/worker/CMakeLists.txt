add_library(eos_skills_worker_protocol protocol.cpp grants.cpp)
add_library(eos_skills_worker_provider provider.cpp)
target_link_libraries(eos_skills_worker_provider PUBLIC eos_internal eos_skills_core eos_skills_worker_protocol eos_dispatch eos_kernel nlohmann_json::nlohmann_json PRIVATE nlohmann_json_schema_validator)
target_link_libraries(eos_skills_worker_protocol PUBLIC eos_internal)

if(ISLAND_BUILD_TESTS)
  add_subdirectory(tests)
endif()

add_executable(eos-skill-worker main.cpp sandbox.cpp)
target_link_libraries(eos-skill-worker PRIVATE eos_skills_native eos_skills_worker_protocol)
install(TARGETS eos-skill-worker RUNTIME DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/island")

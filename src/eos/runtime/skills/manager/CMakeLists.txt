add_library(eos_skills_logging logging.cpp)
target_link_libraries(eos_skills_logging PUBLIC eos_lib)

add_library(eos_skills
  journald.cpp
  manager.cpp
  record_store.cpp
  registry.cpp
  scheduler.cpp
  source_pool.cpp
)

target_link_libraries(eos_skills
  PUBLIC eos_internal eos_skills_core
  PRIVATE eos_lib eos_skills_logging OpenSSL::Crypto
)

add_executable(eos-skill-journal journal_query_main.cpp)
target_link_libraries(eos-skill-journal PRIVATE eos_skills)
install(TARGETS eos-skill-journal RUNTIME DESTINATION bin)

if(ISLAND_BUILD_TESTS)
  add_subdirectory(tests)
endif()

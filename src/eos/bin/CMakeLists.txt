add_executable(eosd
  eos-2607.cpp
)

target_link_libraries(eosd
  PRIVATE eos_init eos_lib
)

add_executable(eos-simulator eos-simulator.cpp)
target_link_libraries(eos-simulator PRIVATE eos_simulator_service eos_lib)

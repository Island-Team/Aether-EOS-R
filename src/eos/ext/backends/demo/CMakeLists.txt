# HAL adapters from the external demo robot API to EOS' backend contract.
option(EOS_BACKEND_DEMO "Build the demo robot backend adapters" ON)

if(EOS_BACKEND_DEMO)
  # EOS-side release latch, admission data, and health semantics shared by
  # every adapter. Robot state and behavior live in the external simulator.
  add_library(eos_backend_demo_core
    core.cpp
  )

  target_include_directories(eos_backend_demo_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
  )

  target_link_libraries(eos_backend_demo_core
    PUBLIC eos_internal
    PUBLIC eos_simulator_ipc_client
    PUBLIC eos_lib  # demo halt-latch logging
  )

  if(ISLAND_BUILD_TESTS)
    add_subdirectory(tests)
  endif()

  add_subdirectory(cam)
  add_subdirectory(mic)
  add_subdirectory(arm)
  add_subdirectory(base)
  add_subdirectory(gimbal)
  add_subdirectory(gripper)
  add_subdirectory(speaker)
endif()

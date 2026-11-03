# HAL adapters from the external demo robot API to EOS' backend contract.
# Every EOS build includes these adapters. A cached legacy OFF option must not
# silently produce a runtime that cannot construct its default backend.
if(DEFINED EOS_BACKEND_DEMO AND NOT EOS_BACKEND_DEMO)
  message(FATAL_ERROR "EOS_BACKEND_DEMO=OFF is unsupported: the demo backend is required by EOS core")
endif()

# EOS-side release latch, admission data, and health semantics shared by
# every adapter. Robot state and behavior live in the external simulator.
add_library(eos_backend_demo_core core.cpp)
target_include_directories(eos_backend_demo_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(eos_backend_demo_core
  PUBLIC eos_internal eos_simulator_ipc_client eos_lib
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

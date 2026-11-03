add_subdirectory(external)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/native/CMakeLists.txt")
  add_subdirectory(native)
endif()

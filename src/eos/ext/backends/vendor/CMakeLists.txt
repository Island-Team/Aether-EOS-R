# Private hardware adapter built against the public Island Base C++ API.
# A sibling checkout is preferred for team development; clean checkouts use
# the pinned upstream revision.
set(EOS_ISLAND_BASE_SOURCE_DIR "${ISLAND_SOURCE_DIR}/../Island-Base" CACHE PATH
    "Island Base source tree used by EOS_BACKEND_VENDOR")

if(NOT TARGET island::pose OR NOT TARGET island::navigation OR NOT TARGET island::manual_motion OR NOT TARGET island::gait OR NOT TARGET island::obstacles)
  if(EXISTS "${EOS_ISLAND_BASE_SOURCE_DIR}/CMakeLists.txt")
    set(BUILD_TESTING OFF)
    add_subdirectory("${EOS_ISLAND_BASE_SOURCE_DIR}"
                     "${CMAKE_BINARY_DIR}/_deps/island_base-build" EXCLUDE_FROM_ALL)
  else()
    FetchContent_Declare(island_base
      GIT_REPOSITORY https://github.com/Island-Team/Island-Base.git
      GIT_TAG 85da65f3395838a8ea836070067d41cec2cba8b2)
    FetchContent_GetProperties(island_base)
    if(NOT island_base_POPULATED)
      FetchContent_Populate(island_base)
      set(BUILD_TESTING OFF)
      add_subdirectory("${island_base_SOURCE_DIR}" "${island_base_BINARY_DIR}"
                       EXCLUDE_FROM_ALL)
    endif()
  endif()
endif()

add_library(eos_backend_vendor EXCLUDE_FROM_ALL
  vendor_base.cpp
  vendor_camera.cpp
  wrist_grasp_stream.cpp
)

find_package(realsense2 REQUIRED)
find_package(PNG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(TURBOJPEG REQUIRED IMPORTED_TARGET libturbojpeg)

target_include_directories(eos_backend_vendor
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(eos_backend_vendor
  PUBLIC eos_hardware_api
  PRIVATE eos_lib island::pose island::navigation island::manual_motion island::gait island::obstacles Boost::boost
          realsense2::realsense2 PNG::PNG PkgConfig::TURBOJPEG
)

add_custom_target(island_pkg_vendor_backend DEPENDS eos_backend_vendor)

if(ISLAND_BUILD_TESTS)
  add_subdirectory(tests)
endif()

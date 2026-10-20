cmake_minimum_required(VERSION 3.22)

project(IslandAether VERSION 1.0.0 LANGUAGES NONE)

# Reuse compilation results across clean reconfigures and branch switches when
# ccache is available. An explicit launcher always takes precedence.
if(NOT DEFINED CMAKE_CXX_COMPILER_LAUNCHER)
  find_program(ISLAND_CCACHE_EXECUTABLE NAMES ccache)
  if(ISLAND_CCACHE_EXECUTABLE)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${ISLAND_CCACHE_EXECUTABLE}"
        CACHE FILEPATH "C++ compiler cache launcher")
    message(STATUS "Using ccache: ${ISLAND_CCACHE_EXECUTABLE}")
  endif()
endif()

# Keep this root a thin product-composition layer. Component-specific build
# logic belongs below src/<component>, allowing the GECA and EOS histories to
# be merged without moving either implementation again.
set(CMAKE_INSTALL_LIBDIR lib CACHE PATH "Object-code library directory")
include(GNUInstallDirs)

option(ISLAND_BUILD_GECA "Expose the GECA Python distribution target" ON)
option(ISLAND_BUILD_EOS "Build the EOS native runtime" ON)
option(ISLAND_BUILD_TESTS "Build and register repository tests" OFF)
option(EOS_NORTH_HTTP "Build the optional EOS north HTTP gateway and OpenAPI artifact" ON)
set(
  ISLAND_INSTALL_ETCDIR "/etc/island"
  CACHE PATH "Island deployment configuration directory"
)

if(NOT DEFINED EOS_CONTRACTS_DIR)
  set(EOS_CONTRACTS_DIR "${CMAKE_SOURCE_DIR}/share")
endif()
set(
  EOS_CONTRACTS_DIR "${EOS_CONTRACTS_DIR}"
  CACHE PATH "Canonical Agent-runtime contract directory used by EOS"
)

if(ISLAND_BUILD_GECA AND EXISTS "${CMAKE_SOURCE_DIR}/src/geca")
  find_program(ISLAND_UV_EXECUTABLE NAMES uv)
  if(ISLAND_UV_EXECUTABLE AND EXISTS "${CMAKE_SOURCE_DIR}/pyproject.toml")
    add_custom_target(
      geca-dist
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/dist"
      COMMAND
        "${ISLAND_UV_EXECUTABLE}" build --out-dir "${CMAKE_BINARY_DIR}/dist"
        "${CMAKE_SOURCE_DIR}"
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      COMMENT "Build the GECA wheel and source distribution"
      USES_TERMINAL
      VERBATIM
    )
  else()
    message(STATUS "GECA source is present but uv or pyproject.toml is unavailable")
  endif()
endif()

if(ISLAND_BUILD_EOS)
  if(EXISTS "${CMAKE_SOURCE_DIR}/src/eos/CMakeLists.txt")
    enable_language(CXX)
    add_subdirectory(src/eos)
  else()
    message(STATUS "EOS source is not present; configuring without the native runtime")
  endif()
endif()

if(ISLAND_BUILD_TESTS)
  enable_testing()
endif()

# Install only roles that exist in the current checkout. This works for this
# standalone EOS history and for the future combined Aether product tree.
set(ISLAND_PUBLIC_COMMANDS)
foreach(command island islandctl geca eosd eos-simulator eos-north-http eos-repl)
  if(NOT EOS_NORTH_HTTP AND (command STREQUAL "eos-north-http" OR command STREQUAL "eos-repl"))
    continue()
  endif()
  if(EXISTS "${CMAKE_SOURCE_DIR}/bin/${command}")
    list(APPEND ISLAND_PUBLIC_COMMANDS "${CMAKE_SOURCE_DIR}/bin/${command}")
  endif()
endforeach()
if(ISLAND_PUBLIC_COMMANDS)
  install(PROGRAMS ${ISLAND_PUBLIC_COMMANDS} DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/share")
  if(EOS_NORTH_HTTP)
    install(DIRECTORY "${CMAKE_SOURCE_DIR}/share/"
      DESTINATION "${CMAKE_INSTALL_DATADIR}/island" PATTERN "skills" EXCLUDE)
  else()
    install(DIRECTORY "${CMAKE_SOURCE_DIR}/share/"
      DESTINATION "${CMAKE_INSTALL_DATADIR}/island"
      PATTERN "skills" EXCLUDE PATTERN "north_http" EXCLUDE)
  endif()
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/docs")
  install(
    DIRECTORY "${CMAKE_SOURCE_DIR}/docs/"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/island"
    FILES_MATCHING PATTERN "*.md"
  )
endif()

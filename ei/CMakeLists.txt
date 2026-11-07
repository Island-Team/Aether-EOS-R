cmake_minimum_required(VERSION 3.24)

project(IsOSEI VERSION 0.2.1 LANGUAGES NONE)

# An EI-only build does not enable a compiled language, so CMake cannot
# infer an architecture-specific library directory before EOS is imported.
set(CMAKE_INSTALL_LIBDIR lib CACHE PATH "Object-code library directory")
include(GNUInstallDirs)

option(ISLAND_BUILD_EGIA "Compatibility option for the EI distribution target" ON)
option(ISLAND_BUILD_EI "Expose the EI Python distribution target" "${ISLAND_BUILD_EGIA}")
option(ISLAND_BUILD_EOS "Build EOS when its native source is present" ON)
option(ISLAND_BUILD_ECOS "Deprecated alias for ISLAND_BUILD_EOS" "${ISLAND_BUILD_EOS}")
option(ISLAND_BUILD_TESTS "Expose the repository verification target" OFF)
option(EGIA_INSTALL_PRIVATE_EXTENSIONS "Install checkout-only operator extensions" OFF)
set(
  ISLAND_INSTALL_ETCDIR "/etc/island"
  CACHE PATH "Island deployment configuration directory"
)

find_program(ISLAND_UV_EXECUTABLE NAMES uv)

if(ISLAND_BUILD_EI)
  if(ISLAND_UV_EXECUTABLE)
    add_custom_target(
      ei-dist
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/dist"
      COMMAND
        "${ISLAND_UV_EXECUTABLE}" build --out-dir "${CMAKE_BINARY_DIR}/dist"
        "${CMAKE_SOURCE_DIR}"
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      COMMENT "Build the EI wheel and source distribution"
      USES_TERMINAL
      VERBATIM
    )
    add_custom_target(egia-dist DEPENDS ei-dist)
  else()
    message(STATUS "uv was not found; the ei-dist target is unavailable")
  endif()
endif()

if(ISLAND_BUILD_EOS AND ISLAND_BUILD_ECOS)
  if(EXISTS "${CMAKE_SOURCE_DIR}/adapters/eos-cxx/CMakeLists.txt")
    enable_language(CXX)
    add_subdirectory(adapters/eos-cxx)
  else()
    message(STATUS "EOS source is not present; configuring the EI-only tree")
  endif()
endif()

if(ISLAND_BUILD_TESTS)
  if(NOT ISLAND_UV_EXECUTABLE)
    message(FATAL_ERROR "ISLAND_BUILD_TESTS requires uv")
  endif()
  enable_testing()
  add_custom_target(
    check
    COMMAND "${ISLAND_UV_EXECUTABLE}" run pytest -q
    COMMAND "${ISLAND_UV_EXECUTABLE}" run ruff check .
    COMMAND "${ISLAND_UV_EXECUTABLE}" run pyright
    COMMAND
      "${CMAKE_COMMAND}" -E env
      "PYTHONPATH=${CMAKE_SOURCE_DIR}/src:${CMAKE_SOURCE_DIR}/libexec"
      "${ISLAND_UV_EXECUTABLE}" run lint-imports
    COMMAND "${CMAKE_SOURCE_DIR}/bin/islandctl" validate
    COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${CMAKE_BINARY_DIR}" --output-on-failure
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Run the Island checkout verification gates"
    USES_TERMINAL
    VERBATIM
  )
  if(TARGET eosd)
    add_dependencies(check eosd)
  endif()
endif()

# Python implementation is released as the wheel produced by ei-dist. This
# manifest maps public commands and non-Python product roles without copying
# src/, tests/, build output, or checkout state into an installation.
set(
  ISLAND_PUBLIC_COMMANDS
  "${CMAKE_SOURCE_DIR}/bin/egia"
  "${CMAKE_SOURCE_DIR}/bin/egia-config"
  "${CMAKE_SOURCE_DIR}/bin/ei"
  "${CMAKE_SOURCE_DIR}/bin/ei-config"
)
foreach(command island islandctl eosd eos-repl)
  if(EXISTS "${CMAKE_SOURCE_DIR}/bin/${command}")
    list(APPEND ISLAND_PUBLIC_COMMANDS "${CMAKE_SOURCE_DIR}/bin/${command}")
  endif()
endforeach()
install(PROGRAMS ${ISLAND_PUBLIC_COMMANDS} DESTINATION "${CMAKE_INSTALL_BINDIR}")
# island-stop is a checkout-only development helper. System services must stop
# only processes they own through their service manager or /run/island state.

if(TARGET eosd)
  set(ISLAND_INSTALLED_EOS_ENABLED true)
else()
  set(ISLAND_INSTALLED_EOS_ENABLED false)
endif()
configure_file(
  cmake/island-installed.toml.in
  "${CMAKE_CURRENT_BINARY_DIR}/island.toml"
  @ONLY
)
install(
  FILES "${CMAKE_CURRENT_BINARY_DIR}/island.toml"
  DESTINATION "${ISLAND_INSTALL_ETCDIR}"
)
install(
  FILES
    etc/egia/egia.toml
    etc/egia/models.toml
    etc/egia/modes.toml
    etc/egia/missions.toml
    etc/egia/prompts.toml
  DESTINATION "${ISLAND_INSTALL_ETCDIR}/egia"
)
install(
  FILES
    etc/egia/profiles/fleet.example.toml
    etc/egia/profiles/live.example.toml
    etc/egia/profiles/sandbox.toml
  DESTINATION "${ISLAND_INSTALL_ETCDIR}/egia/profiles"
)
install(
  FILES
    etc/egia/safety/gate.toml
  DESTINATION "${ISLAND_INSTALL_ETCDIR}/egia/safety"
)
install(
  FILES etc/egia/opt/sandbox.toml
  DESTINATION "${ISLAND_INSTALL_ETCDIR}/egia/opt"
)
install(
  DIRECTORY libexec/island_admin/
  DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/island/island_admin"
  FILES_MATCHING
  PATTERN "*.py"
  PATTERN "__pycache__" EXCLUDE
)
if(EGIA_INSTALL_PRIVATE_EXTENSIONS)
  install(
    DIRECTORY libexec/egia_ext/
    DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/island/egia_ext"
    FILES_MATCHING
    PATTERN "*.py"
    PATTERN "__pycache__" EXCLUDE
  )
endif()

find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
set(
  ISLAND_PRODUCT_CONFIG "${CMAKE_SOURCE_DIR}/etc/island.toml"
  CACHE FILEPATH "Product configuration used to select shared resources"
)
execute_process(
  COMMAND
    "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/resolve_shared_roots.py"
    --prefix "${CMAKE_SOURCE_DIR}" --config "${ISLAND_PRODUCT_CONFIG}"
  RESULT_VARIABLE ISLAND_SHARED_RESULT
  OUTPUT_VARIABLE ISLAND_SHARED_FRAGMENT
  ERROR_VARIABLE ISLAND_SHARED_ERROR
)
if(NOT ISLAND_SHARED_RESULT EQUAL 0)
  message(FATAL_ERROR "Cannot resolve configured shared resources: ${ISLAND_SHARED_ERROR}")
endif()
set(ISLAND_SHARED_FRAGMENT_PATH "${CMAKE_CURRENT_BINARY_DIR}/shared-roots.cmake")
file(WRITE "${ISLAND_SHARED_FRAGMENT_PATH}" "${ISLAND_SHARED_FRAGMENT}")
include("${ISLAND_SHARED_FRAGMENT_PATH}")
foreach(root IN LISTS ISLAND_CONFIGURED_SHARED_ROOTS)
  install(
    DIRECTORY "${root}/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/island"
    PATTERN ".gitkeep" EXCLUDE
  )
endforeach()
# IsOS inventories the exact factory tree used by EGIA's installed launcher.
# Keep the product's share/island tree intact for existing resource consumers.
install(
  DIRECTORY share/defaults/egia/
  DESTINATION "${CMAKE_INSTALL_DATADIR}/defaults/egia"
  PATTERN ".gitkeep" EXCLUDE
)
install(
  DIRECTORY docs/
  DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/island"
  FILES_MATCHING
  PATTERN "*.md"
  PATTERN "tool" EXCLUDE
)

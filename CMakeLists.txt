cmake_minimum_required(VERSION 3.22)

project(IslandAether VERSION 0.2.0 LANGUAGES NONE)

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
option(ISLAND_BUILD_EOS "Build the EOS Rust v2 runtime" ON)
option(ISLAND_BUILD_EOS_CPP "Build the unlaunched C++ hardware and legacy side chain" OFF)
option(ISLAND_BUILD_EOS_SKILL_SDK "Build the standalone native EOS skill SDK" OFF)
option(ISLAND_BUILD_TESTS "Build and register repository tests" OFF)
option(EOS_NORTH_HTTP "Build the old C++ north HTTP gateway when the C++ side chain is enabled" OFF)
set(
  ISLAND_INSTALL_ETCDIR "/etc/island"
  CACHE PATH "Island deployment configuration directory"
)

if(ISLAND_BUILD_TESTS)
  enable_testing()
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME installed_owner_resources
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/tests/eos/test_installed_owner_resources.py"
      "${CMAKE_COMMAND}" "${CMAKE_SOURCE_DIR}")
endif()

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

if(ISLAND_BUILD_EOS_SKILL_SDK OR ISLAND_BUILD_EOS_CPP)
  enable_language(CXX)
  add_subdirectory(src/eos/skill_sdk)
endif()

if(ISLAND_BUILD_EOS_CPP)
  if(EXISTS "${CMAKE_SOURCE_DIR}/src/eos/CMakeLists.txt")
    add_subdirectory(src/eos)
  else()
    message(FATAL_ERROR "C++ EOS source is not present")
  endif()
endif()

if(ISLAND_BUILD_EOS)
  find_program(ISLAND_CARGO_EXECUTABLE NAMES cargo
    HINTS "$ENV{HOME}/.cargo/bin" REQUIRED)
  set(ISLAND_RUST_TARGET_DIR "${CMAKE_BINARY_DIR}/cargo-target")
  set(ISLAND_RUST_BINARIES aether-runtime aether-profile aether-http-bridge aether-config-server aether-tui)
  add_custom_target(eos_rust ALL
    COMMAND "${CMAKE_COMMAND}" -E env "CARGO_TARGET_DIR=${ISLAND_RUST_TARGET_DIR}"
      "${ISLAND_CARGO_EXECUTABLE}" build --release --workspace
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Building EOS v2 Rust workspace"
    USES_TERMINAL VERBATIM)
  foreach(binary IN LISTS ISLAND_RUST_BINARIES)
    install(PROGRAMS "${ISLAND_RUST_TARGET_DIR}/release/${binary}"
      DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/island")
  endforeach()
  if(ISLAND_BUILD_TESTS)
    add_test(NAME installed_rust_wrappers
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/eos/test_installed_rust_wrappers.py"
        "${CMAKE_COMMAND}" "${CMAKE_BINARY_DIR}" "${CMAKE_SOURCE_DIR}")
  endif()
endif()

# Install only roles that exist in the current checkout. This works for this
# standalone EOS history and for the future combined Aether product tree.
set(ISLAND_PUBLIC_COMMANDS)
foreach(command island islandctl island-pkg island-launch geca eos eosd eos-config eos-north-http eos-cli eos-repl aether-launch aether-profile)
  if(command STREQUAL "island-launch" AND NOT ISLAND_BUILD_EOS_CPP)
    continue()
  endif()
  if(EXISTS "${CMAKE_SOURCE_DIR}/bin/${command}")
    list(APPEND ISLAND_PUBLIC_COMMANDS "${CMAKE_SOURCE_DIR}/bin/${command}")
  endif()
endforeach()
if(ISLAND_PUBLIC_COMMANDS)
  install(PROGRAMS ${ISLAND_PUBLIC_COMMANDS} DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

if(ISLAND_BUILD_EOS_CPP AND EXISTS "${CMAKE_SOURCE_DIR}/etc/runtime/stack.toml")
  install(FILES "${CMAKE_SOURCE_DIR}/etc/runtime/stack.toml"
    DESTINATION "${ISLAND_INSTALL_ETCDIR}/runtime")
endif()

if(ISLAND_BUILD_EOS_CPP AND EXISTS "${CMAKE_SOURCE_DIR}/etc/ext")
  install(DIRECTORY "${CMAKE_SOURCE_DIR}/etc/ext/"
    DESTINATION "${ISLAND_INSTALL_ETCDIR}/ext")
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/share/eos/contracts")
  install(DIRECTORY "${CMAKE_SOURCE_DIR}/share/eos/contracts"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/island/eos")
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/share/eos/schemas")
  install(DIRECTORY "${CMAKE_SOURCE_DIR}/share/eos/schemas"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/island/eos")
endif()
if(ISLAND_BUILD_EOS_CPP AND EOS_NORTH_HTTP AND EXISTS "${CMAKE_SOURCE_DIR}/share/ext/north-http/openapi")
  install(DIRECTORY "${CMAKE_SOURCE_DIR}/share/ext/north-http/openapi"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/island/ext/north-http")
endif()

foreach(language IN ITEMS docs docs_cn)
  if(EXISTS "${CMAKE_SOURCE_DIR}/${language}")
    install(DIRECTORY "${CMAKE_SOURCE_DIR}/${language}/"
      DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/island/${language}"
      FILES_MATCHING PATTERN "*.md")
  endif()
endforeach()
if(ISLAND_BUILD_EOS)
  install(FILES
    "${CMAKE_SOURCE_DIR}/etc/eos/backends/mock-cxx.toml"
    "${CMAKE_SOURCE_DIR}/etc/eos/backends/mujoco-socket.toml"
    DESTINATION "${ISLAND_INSTALL_ETCDIR}/eos/graphs")
endif()

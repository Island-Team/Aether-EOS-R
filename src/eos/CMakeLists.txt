set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(EOS_REPL "Build the northbound REPL client (eos-repl)" ON)

get_filename_component(ISLAND_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(EOS_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(EOS_INCLUDE_DIR "${EOS_SOURCE_DIR}/include")
if(NOT DEFINED EOS_CONTRACTS_DIR)
  set(EOS_CONTRACTS_DIR "${ISLAND_SOURCE_DIR}/share")
endif()

# JSON Schemas are the reviewed source. Python only validates their complete
# set and checks the deterministic inventory consumed by the native runtime.
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Demo backends are clients of the standalone simulator. Tests that exercise
# them run through this helper so every executable receives a clean simulator
# state and socket while CTest serializes the simulator's fixed web port.
function(eos_add_simulator_test test_name target_name)
  add_test(
    NAME "${test_name}"
    COMMAND
      "${Python3_EXECUTABLE}" "${ISLAND_SOURCE_DIR}/tests/eos/run_with_simulator.py"
      --simulator "$<TARGET_FILE:eos-simulator>"
      --etc "${ISLAND_SOURCE_DIR}/etc/eos"
      --contracts "${EOS_CONTRACTS_DIR}"
      -- "$<TARGET_FILE:${target_name}>"
  )
  set_tests_properties("${test_name}" PROPERTIES
    RESOURCE_LOCK eos_simulator_port_16004
    TIMEOUT 300
  )
  set_property(GLOBAL APPEND PROPERTY EOS_SIMULATOR_TEST_TARGETS "${target_name}")
endfunction()

file(GLOB_RECURSE EOS_CONTRACT_PROTO_FILES CONFIGURE_DEPENDS
  "${ISLAND_SOURCE_DIR}/share/contracts/eos/actions/*.proto"
  "${ISLAND_SOURCE_DIR}/share/contracts/eos/body/*.proto"
  "${ISLAND_SOURCE_DIR}/share/contracts/eos/world/*.proto"
)
add_custom_target(eos_contracts ALL
  COMMAND "${Python3_EXECUTABLE}" "${ISLAND_SOURCE_DIR}/scripts/generate_eos_contracts.py"
          --proto-root "${ISLAND_SOURCE_DIR}/share/contracts"
          --output "${EOS_CONTRACTS_DIR}/ext/eos" --check
  DEPENDS
    "${ISLAND_SOURCE_DIR}/scripts/generate_eos_contracts.py"
    "${ISLAND_SOURCE_DIR}/scripts/contract_manifest.py"
    "${EOS_CONTRACTS_DIR}/ext/eos/contracts.manifest.json"
    ${EOS_CONTRACT_PROTO_FILES}
  COMMENT "Validating generated EOS JSON Schemas and contract manifest"
  VERBATIM
)
if(EOS_NORTH_HTTP)
  add_custom_target(eos_north_http_openapi ALL
    COMMAND "${Python3_EXECUTABLE}" "${ISLAND_SOURCE_DIR}/scripts/generate_north_http_openapi.py"
            --proto-root "${ISLAND_SOURCE_DIR}/share/contracts"
            --runtime-root "${EOS_CONTRACTS_DIR}/ext/eos"
            --output "${EOS_CONTRACTS_DIR}/ext/north_http/openapi.json" --check
    DEPENDS eos_contracts
      "${ISLAND_SOURCE_DIR}/scripts/generate_north_http_openapi.py"
      "${EOS_CONTRACTS_DIR}/ext/north_http/openapi.json"
      "${ISLAND_SOURCE_DIR}/share/contracts/eos/external/http/http.proto"
    COMMENT "Validating generated north HTTP OpenAPI"
    VERBATIM)
endif()

if(NOT DEFINED ISLAND_BUILD_TESTS)
  set(ISLAND_BUILD_TESTS OFF)
endif()
if(ISLAND_BUILD_TESTS)
  enable_testing()
  # The current EGIA-bearing product root exposes its own `check` target but
  # does not call enable_testing(). Register it there as a deferred command so
  # `ctest --test-dir <combined-build>` discovers this nested native suite.
  cmake_language(DEFER DIRECTORY "${ISLAND_SOURCE_DIR}" CALL enable_testing)
endif()

add_library(eos_hardware_api INTERFACE)
target_include_directories(eos_hardware_api INTERFACE "${EOS_INCLUDE_DIR}")

add_library(eos_internal INTERFACE)
target_include_directories(eos_internal INTERFACE "${ISLAND_SOURCE_DIR}")
target_link_libraries(eos_internal INTERFACE eos_hardware_api)

find_package(Boost 1.74 REQUIRED)
find_package(OpenSSL REQUIRED)

# --- third-party dependencies ---------------------------------------------
# System packages win when present (apt: nlohmann-json3-dev,
# libtomlplusplus-dev); otherwise FetchContent builds them from mirrors so a
# dependency-free checkout still configures. The shared cache dir keeps
# worktree builds from re-cloning per build tree. Mirrors strip tags on
# clone, so pins that need them use commit hashes (tag named in comment).
set(FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/_deps" CACHE PATH
    "FetchContent dependency directory for the combined Island build")
include(FetchContent)

find_package(nlohmann_json 3.11 QUIET)
if(NOT nlohmann_json_FOUND)
  # Not found via apt, or found but below the 3.11 floor (e.g. Ubuntu 22.04
  # jammy ships 3.10.5 — this is the normal path on Jetson/jammy).
  message(STATUS "eos: nlohmann_json >=3.11 not found on system, fetching via FetchContent")
  set(JSON_BuildTests OFF)
  FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://gitcode.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(nlohmann_json)
  set(EOS_NLOHMANN_JSON_DIR "" CACHE INTERNAL
      "nlohmann_json package config dir; empty when fetched")
  set(EOS_NLOHMANN_JSON_SOURCE_DIR "${nlohmann_json_SOURCE_DIR}" CACHE INTERNAL
      "nlohmann_json source tree populated by FetchContent; empty for a system package")
else()
  message(STATUS "eos: using system nlohmann_json")
  set(EOS_NLOHMANN_JSON_DIR "${nlohmann_json_DIR}" CACHE INTERNAL
      "nlohmann_json package config dir; empty when fetched")
  set(EOS_NLOHMANN_JSON_SOURCE_DIR "" CACHE INTERNAL
      "nlohmann_json source tree populated by FetchContent; empty for a system package")
endif()

find_package(tomlplusplus QUIET)
if(NOT tomlplusplus_FOUND)
  # No apt package at all on Ubuntu 22.04 jammy (added upstream in 24.04
  # noble) — this is the normal path on Jetson/jammy.
  message(STATUS "eos: tomlplusplus not found on system, fetching via FetchContent")
  FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://gitcode.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(tomlplusplus)
else()
  message(STATUS "eos: using system tomlplusplus")
endif()

if(EOS_REPL)
  find_package(replxx CONFIG QUIET)
  if(NOT replxx_FOUND AND NOT TARGET replxx::replxx)
    message(STATUS "eos: replxx not found on system, fetching via FetchContent")
    set(REPLXX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(REPLXX_BUILD_PACKAGE OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(replxx
      GIT_REPOSITORY https://github.com/AmokHuginnsson/replxx.git
      GIT_TAG 2b248467112cbbc16f63fde38230fd7c7ffc55f1)  # release-0.0.4
    FetchContent_GetProperties(replxx)
    if(NOT replxx_POPULATED)
      FetchContent_Populate(replxx)
      add_subdirectory("${replxx_SOURCE_DIR}" "${replxx_BINARY_DIR}" EXCLUDE_FROM_ALL)
    endif()
  else()
    message(STATUS "eos: using system replxx")
  endif()
endif()

find_package(nlohmann_json_schema_validator QUIET)
if(NOT nlohmann_json_schema_validator_FOUND)
  message(STATUS "eos: nlohmann_json_schema_validator not found on system, fetching via FetchContent")
  set(JSON_VALIDATOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(JSON_VALIDATOR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(JSON_VALIDATOR_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(json_schema_validator
    GIT_REPOSITORY https://gitcode.com/pboettch/json-schema-validator.git
    GIT_TAG 349cba9f7e3cb423bbc1811bdd9f6770f520b468)  # v2.3.0
  FetchContent_MakeAvailable(json_schema_validator)
else()
  message(STATUS "eos: using system nlohmann_json_schema_validator")
endif()

find_package(gRPC CONFIG QUIET NO_SYSTEM_ENVIRONMENT_PATH)
if(NOT gRPC_FOUND)
  # Ubuntu 22.04's libgrpc++-dev ships pkg-config metadata but no gRPC CMake
  # package. Prefer it over a source build when the matching Linux plugin and
  # reflection library are installed.
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(GRPCPP QUIET IMPORTED_TARGET grpc++)
    find_program(EOS_SYSTEM_GRPC_CPP_PLUGIN NAMES grpc_cpp_plugin)
    find_library(EOS_SYSTEM_GRPC_REFLECTION NAMES grpc++_reflection)
    if(GRPCPP_FOUND AND EOS_SYSTEM_GRPC_CPP_PLUGIN AND EOS_SYSTEM_GRPC_REFLECTION)
      add_library(gRPC::grpc++ ALIAS PkgConfig::GRPCPP)
      add_library(gRPC::grpc++_reflection UNKNOWN IMPORTED)
      set_target_properties(gRPC::grpc++_reflection PROPERTIES
        IMPORTED_LOCATION "${EOS_SYSTEM_GRPC_REFLECTION}"
        INTERFACE_LINK_LIBRARIES PkgConfig::GRPCPP)
      add_executable(gRPC::grpc_cpp_plugin IMPORTED GLOBAL)
      set_target_properties(gRPC::grpc_cpp_plugin PROPERTIES
        IMPORTED_LOCATION "${EOS_SYSTEM_GRPC_CPP_PLUGIN}")
      set(gRPC_FOUND TRUE)
      message(STATUS "eos: using system gRPC from pkg-config")
    endif()
  endif()
endif()
if(gRPC_FOUND)
  find_package(Protobuf CONFIG QUIET)
  if(NOT Protobuf_FOUND)
    find_package(Protobuf REQUIRED)
  endif()
endif()
if(NOT gRPC_FOUND)
  message(STATUS "eos: protobuf/gRPC not found on system, fetching gRPC and bundled dependencies")
  set(gRPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(gRPC_BUILD_GRPC_CPP_PLUGIN ON CACHE BOOL "" FORCE)
  set(gRPC_BUILD_GRPC_CSHARP_PLUGIN OFF CACHE BOOL "" FORCE)
  set(gRPC_BUILD_GRPC_NODE_PLUGIN OFF CACHE BOOL "" FORCE)
  set(gRPC_BUILD_GRPC_OBJECTIVE_C_PLUGIN OFF CACHE BOOL "" FORCE)
  set(gRPC_BUILD_GRPC_PHP_PLUGIN OFF CACHE BOOL "" FORCE)
  set(gRPC_BUILD_GRPC_PYTHON_PLUGIN OFF CACHE BOOL "" FORCE)
  set(gRPC_BUILD_GRPC_RUBY_PLUGIN OFF CACHE BOOL "" FORCE)
  set(gRPC_ZLIB_PROVIDER package CACHE STRING "" FORCE)
  set(RE2_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(gRPC_INSTALL OFF CACHE BOOL "" FORCE)
  set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
  set(utf8_range_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(grpc
    GIT_REPOSITORY https://github.com/grpc/grpc.git
    GIT_TAG v1.62.2
    GIT_SHALLOW TRUE
    # FetchContent's recursive default also downloads unused benchmark,
    # bloaty, envoy, and telemetry repositories. Keep first-time edge builds
    # bounded to the module providers reached by the EOS C++ targets.
    GIT_SUBMODULES
      third_party/abseil-cpp
      third_party/boringssl-with-bazel
      third_party/cares/cares
      third_party/protobuf
      third_party/re2
    GIT_SUBMODULES_RECURSE TRUE)
  FetchContent_GetProperties(grpc)
  if(NOT grpc_POPULATED)
    FetchContent_Populate(grpc)
    # gRPC declares many optional libraries and examples as default targets.
    # Build only targets reached through EOS to keep native edge builds practical.
    add_subdirectory("${grpc_SOURCE_DIR}" "${grpc_BINARY_DIR}" EXCLUDE_FROM_ALL)
  endif()
endif()

if(TARGET grpc++ AND NOT TARGET gRPC::grpc++)
  add_library(gRPC::grpc++ ALIAS grpc++)
endif()
if(TARGET grpc++_reflection AND NOT TARGET gRPC::grpc++_reflection)
  add_library(gRPC::grpc++_reflection ALIAS grpc++_reflection)
endif()

# Public developer contract for trusted native skill modules. Keep this target
# owned by EOS rather than by the example/deployment skill source tree.
add_library(eos_skill_api INTERFACE)
target_include_directories(eos_skill_api INTERFACE
  "$<BUILD_INTERFACE:${ISLAND_SOURCE_DIR}/src/eos/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
set_target_properties(eos_skill_api PROPERTIES EXPORT_NAME SkillABI)
add_library(EOS::SkillABI ALIAS eos_skill_api)

add_library(eos_skill_cpp INTERFACE)
get_target_property(EOS_NLOHMANN_INCLUDE_DIRS nlohmann_json::nlohmann_json
                    INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(eos_skill_cpp INTERFACE
  "$<BUILD_INTERFACE:${EOS_NLOHMANN_INCLUDE_DIRS}>"
)
target_link_libraries(eos_skill_cpp INTERFACE
  eos_skill_api
  "$<INSTALL_INTERFACE:nlohmann_json::nlohmann_json>"
)
set_target_properties(eos_skill_cpp PROPERTIES EXPORT_NAME SkillCpp)
add_library(EOS::SkillCpp ALIAS eos_skill_cpp)

set(EOS_SKILL_EXPORTS_MAP "${CMAKE_CURRENT_LIST_DIR}/cmake/EOSSkillExports.map")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/EOSSkillHelpers.cmake")

# Island-maintained shared skills use the same public ABI as deployment-owned
# modules and produce loadable *.skill.so artifacts outside the runtime binary.
add_subdirectory("${ISLAND_SOURCE_DIR}/share/skills"
                 "${CMAKE_CURRENT_BINARY_DIR}/shared-skills")

# Let constrained hosts compile heavyweight source fallbacks before the EOS
# graph. System/imported dependencies make this target an inexpensive no-op.
add_custom_target(eos_dependencies)
foreach(dependency_target IN ITEMS
    grpc++
    grpc++_reflection
    grpc_cpp_plugin
    nlohmann_json_schema_validator
    replxx)
  if(TARGET "${dependency_target}")
    add_dependencies(eos_dependencies "${dependency_target}")
  endif()
endforeach()

add_subdirectory(api)
add_subdirectory(lib)
if(EOS_NORTH_HTTP)
  add_subdirectory(ext/north_http)
endif()
add_subdirectory(ext/decision)
add_subdirectory(ext/simulator)
add_subdirectory(ext/backends)
add_subdirectory(runtime/perception)
add_subdirectory(runtime/perception/body)
add_subdirectory(runtime/hardware)
add_subdirectory(runtime/dispatch)
add_subdirectory(runtime/skills)
add_subdirectory(runtime/actionlist)
add_subdirectory(runtime/models)
add_subdirectory(runtime/store)
add_subdirectory(ext/models)
add_subdirectory(runtime/init)
add_subdirectory(runtime/northbound)
add_subdirectory(bin)
if(EOS_NORTH_HTTP)
  add_subdirectory(ext/repl)
endif()

if(ISLAND_BUILD_TESTS)
  add_subdirectory("${ISLAND_SOURCE_DIR}/tests/eos" "${CMAKE_CURRENT_BINARY_DIR}/tests")
  get_property(EOS_SIMULATOR_TEST_TARGETS GLOBAL PROPERTY EOS_SIMULATOR_TEST_TARGETS)
  foreach(test_target IN LISTS EOS_SIMULATOR_TEST_TARGETS)
    add_dependencies("${test_target}" eos-simulator)
  endforeach()
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS eosd RUNTIME DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/island")
install(TARGETS eos-simulator RUNTIME DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/island")
if(EOS_NORTH_HTTP)
  install(TARGETS eos_north_http RUNTIME DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/island")
endif()
if(TARGET eos_repl)
  install(TARGETS eos_repl RUNTIME DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/island")
endif()
install(
  DIRECTORY "${ISLAND_SOURCE_DIR}/src/eos/include/eos/skill"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/eos"
)
configure_package_config_file(
  "${CMAKE_CURRENT_LIST_DIR}/cmake/EOSSkillConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/EOSSkillConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/EOSSkill"
)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/EOSSkillConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion
)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/EOSSkillConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/EOSSkillConfigVersion.cmake"
  "${CMAKE_CURRENT_LIST_DIR}/cmake/EOSSkillHelpers.cmake"
  "${CMAKE_CURRENT_LIST_DIR}/cmake/EOSSkillExports.map"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/EOSSkill"
)
if(DEFINED ISLAND_INSTALL_ETCDIR)
  install(
    DIRECTORY "${ISLAND_SOURCE_DIR}/etc/eos/"
    DESTINATION "${ISLAND_INSTALL_ETCDIR}/eos"
    PATTERN "eos.toml" EXCLUDE
  )
endif()

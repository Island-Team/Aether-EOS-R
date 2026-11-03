# The native skill ABI is an active developer package, independent of the
# unlaunched historical C++ process graph in src/eos.
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(FetchContent)

set(ISLAND_SOURCE_DIR "${CMAKE_SOURCE_DIR}")
find_package(nlohmann_json 3.11 QUIET)
if(NOT nlohmann_json_FOUND AND NOT TARGET nlohmann_json::nlohmann_json)
  set(JSON_BuildTests OFF)
  FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://gitcode.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(nlohmann_json)
  set(EOS_NLOHMANN_JSON_DIR "" CACHE INTERNAL "nlohmann_json package config dir")
  set(EOS_NLOHMANN_JSON_SOURCE_DIR "${nlohmann_json_SOURCE_DIR}" CACHE INTERNAL
      "nlohmann_json source tree")
else()
  if(NOT nlohmann_json_FOUND AND DEFINED nlohmann_json_SOURCE_DIR
      AND EXISTS "${nlohmann_json_SOURCE_DIR}/include/nlohmann/json.hpp")
    set(EOS_NLOHMANN_JSON_DIR "" CACHE INTERNAL "nlohmann_json package config dir")
    set(EOS_NLOHMANN_JSON_SOURCE_DIR "${nlohmann_json_SOURCE_DIR}" CACHE INTERNAL
        "nlohmann_json source tree")
  else()
    set(EOS_NLOHMANN_JSON_DIR "${nlohmann_json_DIR}" CACHE INTERNAL
        "nlohmann_json package config dir")
    set(EOS_NLOHMANN_JSON_SOURCE_DIR "" CACHE INTERNAL "nlohmann_json source tree")
  endif()
endif()
if(EOS_NLOHMANN_JSON_SOURCE_DIR)
  set(EOS_SKILL_BUNDLED_NLOHMANN TRUE)
  install(DIRECTORY "${EOS_NLOHMANN_JSON_SOURCE_DIR}/include/nlohmann"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
else()
  set(EOS_SKILL_BUNDLED_NLOHMANN FALSE)
endif()

add_library(eos_skill_api INTERFACE)
target_include_directories(eos_skill_api INTERFACE
  "$<BUILD_INTERFACE:${ISLAND_SOURCE_DIR}/src/eos/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
set_target_properties(eos_skill_api PROPERTIES EXPORT_NAME SkillABI)
add_library(EOS::SkillABI ALIAS eos_skill_api)

add_library(eos_skill_cpp INTERFACE)
get_target_property(EOS_NLOHMANN_INCLUDE_DIRS nlohmann_json::nlohmann_json
                    INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(eos_skill_cpp INTERFACE
  "$<BUILD_INTERFACE:${EOS_NLOHMANN_INCLUDE_DIRS}>")
target_link_libraries(eos_skill_cpp INTERFACE eos_skill_api
  "$<INSTALL_INTERFACE:nlohmann_json::nlohmann_json>")
set_target_properties(eos_skill_cpp PROPERTIES EXPORT_NAME SkillCpp)
add_library(EOS::SkillCpp ALIAS eos_skill_cpp)

set(EOS_SKILL_EXPORTS_MAP "${ISLAND_SOURCE_DIR}/src/eos/cmake/EOSSkillExports.map")
include("${ISLAND_SOURCE_DIR}/src/eos/cmake/EOSSkillHelpers.cmake")

add_library(eos_skill_sdk SHARED
  "${ISLAND_SOURCE_DIR}/src/eos/runtime/skills/native/sdk_support.cpp")
target_link_libraries(eos_skill_sdk PUBLIC eos_skill_api)
set_target_properties(eos_skill_sdk PROPERTIES VERSION "${PROJECT_VERSION}" SOVERSION 2)
add_library(EOS::SkillSDK ALIAS eos_skill_sdk)
install(TARGETS eos_skill_sdk LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY "${ISLAND_SOURCE_DIR}/src/eos/include/eos/skill"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/eos")
configure_package_config_file(
  "${ISLAND_SOURCE_DIR}/src/eos/cmake/EOSSkillConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/EOSSkillConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/EOSSkill")
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/EOSSkillConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMajorVersion)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/EOSSkillConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/EOSSkillConfigVersion.cmake"
  "${ISLAND_SOURCE_DIR}/src/eos/cmake/EOSSkillHelpers.cmake"
  "${ISLAND_SOURCE_DIR}/src/eos/cmake/EOSSkillExports.map"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/EOSSkill")

if(ISLAND_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  find_package(GTest REQUIRED)
  add_executable(eos_skill_sdk_tests
    "${ISLAND_SOURCE_DIR}/src/eos/runtime/skills/native/tests/sdk_test.cpp"
    "${ISLAND_SOURCE_DIR}/src/eos/runtime/skills/native/tests/sdk_support_test.cpp")
  target_link_libraries(eos_skill_sdk_tests PRIVATE
    eos_skill_cpp eos_skill_sdk GTest::gtest GTest::gtest_main)
  add_test(NAME eos_skill_sdk_tests COMMAND eos_skill_sdk_tests)
  find_program(EOS_NM_EXECUTABLE NAMES nm REQUIRED)
  add_test(NAME installed_skill_sdk
    COMMAND "${Python3_EXECUTABLE}" "${ISLAND_SOURCE_DIR}/tests/eos/test_skill_package.py"
      "${CMAKE_COMMAND}" "${EOS_NM_EXECUTABLE}" "${ISLAND_SOURCE_DIR}"
      "${CMAKE_BINARY_DIR}")
  set_tests_properties(installed_skill_sdk PROPERTIES TIMEOUT 120)
endif()

cmake_minimum_required(VERSION 3.22)
project(EOSSkillPackageConsumer LANGUAGES CXX)

find_package(EOSSkill CONFIG REQUIRED)
eos_add_skill(
  TARGET standalone_skill
  SOURCES standalone_skill.cpp
  OUTPUT_NAME standalone
  NO_INSTALL
)

add_executable(standalone_loader loader.cpp)
target_link_libraries(standalone_loader PRIVATE EOS::SkillABI ${CMAKE_DL_LIBS})

include(CMakeParseArguments)
include(GNUInstallDirs)

function(eos_add_skill)
  set(options NO_INSTALL)
  set(one_value_args TARGET OUTPUT_NAME INSTALL_DESTINATION)
  set(multi_value_args SOURCES)
  cmake_parse_arguments(EOS_SKILL "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

  if(NOT EOS_SKILL_TARGET OR NOT EOS_SKILL_SOURCES)
    message(FATAL_ERROR "eos_add_skill requires TARGET and SOURCES")
  endif()
  if(NOT TARGET EOS::SkillCpp)
    message(FATAL_ERROR "eos_add_skill requires EOS::SkillCpp")
  endif()
  if(NOT EOS_SKILL_OUTPUT_NAME)
    set(EOS_SKILL_OUTPUT_NAME "${EOS_SKILL_TARGET}")
  endif()
  if(NOT EOS_SKILL_INSTALL_DESTINATION)
    set(EOS_SKILL_INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/island/skills")
  endif()
  if(NOT EOS_SKILL_EXPORTS_MAP OR NOT EXISTS "${EOS_SKILL_EXPORTS_MAP}")
    message(FATAL_ERROR "EOS skill export map is unavailable")
  endif()

  add_library(${EOS_SKILL_TARGET} MODULE ${EOS_SKILL_SOURCES})
  target_link_libraries(${EOS_SKILL_TARGET} PRIVATE EOS::SkillCpp)
  target_link_options(${EOS_SKILL_TARGET} PRIVATE
    "LINKER:--version-script=${EOS_SKILL_EXPORTS_MAP}"
  )
  set_target_properties(${EOS_SKILL_TARGET} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "${EOS_SKILL_OUTPUT_NAME}"
    SUFFIX ".skill.so"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/skills"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    LINK_DEPENDS "${EOS_SKILL_EXPORTS_MAP}"
  )
  add_custom_command(TARGET ${EOS_SKILL_TARGET} PRE_LINK
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${EOS_SKILL_TARGET}>"
    VERBATIM
  )
  if(NOT EOS_SKILL_NO_INSTALL)
    install(TARGETS ${EOS_SKILL_TARGET} LIBRARY DESTINATION "${EOS_SKILL_INSTALL_DESTINATION}")
  endif()
endfunction()

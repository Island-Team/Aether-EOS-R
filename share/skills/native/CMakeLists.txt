add_library(island_native_skills
  skills.cpp
  common/assist_support.cpp
  common/primitive_support.cpp
  head_find_trash/head_find_trash.cpp
  head_find_trash_navigation_resume/head_find_trash_navigation_resume.cpp
  navigate_pick_resume/navigate_pick_resume.cpp
  navigate_trash/navigate_trash.cpp
  navigate_waypoint/navigate_waypoint.cpp
  observe_human/observe_human.cpp
  owner_interaction/owner_interaction.cpp
  pick_trash/pick_trash.cpp
  register_host/name_parser.cpp
  register_host/register_host.cpp
)

target_compile_features(island_native_skills PUBLIC cxx_std_20)
target_link_libraries(island_native_skills
  PUBLIC eos_internal eos_skills_builtin eos_skills_core nlohmann_json::nlohmann_json
)

#include "share/skills/native/skills.h"

#include "share/skills/native/head_find_trash/head_find_trash.h"
#include "share/skills/native/head_find_trash_navigation_resume/head_find_trash_navigation_resume.h"
#include "share/skills/native/navigate_pick_resume/navigate_pick_resume.h"
#include "share/skills/native/navigate_trash/navigate_trash.h"
#include "share/skills/native/navigate_waypoint/navigate_waypoint.h"
#include "share/skills/native/owner_interaction/owner_interaction.h"
#include "share/skills/native/pick_trash/pick_trash.h"
#include "share/skills/native/register_host/register_host.h"

namespace island::skills::native {

void RegisterObserveHuman(::eos::runtime::skills::builtin::Registry& registry);

void RegisterSkills(::eos::runtime::skills::builtin::Registry& registry) {
  RegisterObserveHuman(registry);
  RegisterHeadFindTrash(registry);
  head_find_trash_navigation_resume::RegisterSkill(registry);
  navigate_pick_resume::RegisterSkill(registry);
  navigate_trash::RegisterSkills(registry);
  navigate_waypoint::RegisterSkill(registry);
  owner_interaction::RegisterSkills(registry);
  RegisterPickTrash(registry);
  register_host::RegisterSkill(registry);
}

}  // namespace island::skills::native

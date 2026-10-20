#pragma once

namespace eos::runtime::skills::builtin {
class Registry;
}

namespace island::skills::native {

void RegisterSkills(::eos::runtime::skills::builtin::Registry& registry);

}  // namespace island::skills::native

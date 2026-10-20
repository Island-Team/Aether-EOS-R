#pragma once

namespace eos::runtime::skills::builtin {
class Registry;
}

namespace island::skills::native::register_host {

void RegisterSkill(::eos::runtime::skills::builtin::Registry& registry);

}  // namespace island::skills::native::register_host

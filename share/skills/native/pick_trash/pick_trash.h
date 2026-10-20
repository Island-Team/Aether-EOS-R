#pragma once

namespace eos::runtime::skills::builtin {
class Registry;
class Execution;
struct Outcome;
template <typename T>
class Task;
}

namespace island::skills::native {

::eos::runtime::skills::builtin::Task<::eos::runtime::skills::builtin::Outcome> RunPickTrash(
    ::eos::runtime::skills::builtin::Execution& execution);
void RegisterPickTrash(::eos::runtime::skills::builtin::Registry& registry);

}  // namespace island::skills::native

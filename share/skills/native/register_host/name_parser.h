#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace island::skills::native::register_host {

std::optional<std::string> ParseIntroducedName(std::string_view text);

}  // namespace island::skills::native::register_host

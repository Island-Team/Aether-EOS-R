#pragma once

#include <filesystem>

namespace eos::runtime::skills::worker {

// Apply the mandatory package-worker boundary before loading any skill library.
// Throws when the host cannot provide the boundary.
void ApplySandbox(const std::filesystem::path& skill_directory);

}  // namespace eos::runtime::skills::worker

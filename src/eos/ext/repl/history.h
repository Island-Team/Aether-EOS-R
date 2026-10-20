/// Per-user persistent history path resolution.
#pragma once

#include <string>

namespace eos::ext::repl {

std::string ResolveHistoryPath();
bool PrepareHistoryPath(const std::string& path, std::string* warning);
bool SecureHistoryFile(const std::string& path, std::string* warning);

}  // namespace eos::ext::repl

/// Testable JSON-object editing loop used by interactive argument entry.
#pragma once

#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace eos::ext::repl {

using JsonLineReader = std::function<bool(const std::string& preload, std::string* submitted)>;
using JsonErrorWriter = std::function<void(const std::string& message)>;

std::optional<nlohmann::json> ReadJsonObject(const JsonLineReader& read,
                                             const JsonErrorWriter& write_error,
                                             const std::string& initial = "{}");

}  // namespace eos::ext::repl

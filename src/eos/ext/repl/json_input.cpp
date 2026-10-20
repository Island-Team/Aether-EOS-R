#include "json_input.h"

namespace eos::ext::repl {

std::optional<nlohmann::json> ReadJsonObject(const JsonLineReader& read,
                                             const JsonErrorWriter& write_error,
                                             const std::string& initial) {
  std::string input;
  std::string preload = initial;
  while (read(preload, &input)) {
    try {
      nlohmann::json arguments = nlohmann::json::parse(input);
      if (arguments.is_object()) return arguments;
      write_error("JSON arguments must be an object");
    } catch (const std::exception& error) {
      write_error("invalid JSON: " + std::string(error.what()));
    }
    preload = input;
  }
  return std::nullopt;
}

}  // namespace eos::ext::repl

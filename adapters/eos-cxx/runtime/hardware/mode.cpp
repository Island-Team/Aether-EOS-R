#include "mode.h"

#include <stdexcept>
#include <toml++/toml.hpp>

namespace eos::runtime::hardware_runtime {
namespace {

/// Throws the mode-level parse error: file name first, then the problem.
[[noreturn]] void Fail(const std::string& path, const std::string& what) {
  throw std::runtime_error("mode '" + path + "': " + what);
}

}  // namespace

ModeConfig ParseMode(std::string_view text, const std::string& path) {
  toml::table root;
  try {
    root = toml::parse(text, path);
  } catch (const toml::parse_error& error) {
    throw std::runtime_error("mode '" + path + "': " + std::string(error.description()));
  }

  const auto* section = root.get("mode");
  if (section == nullptr) {
    Fail(path, "missing required table '[mode]'");
  }
  const auto* mode = section->as_table();
  if (mode == nullptr) {
    Fail(path, "key 'mode' must be a table");
  }

  ModeConfig config;
  if (mode->contains("tools")) {
    Fail(path, "unknown key 'tools' in [mode]; use [mode.primitives]");
  }
  const auto* bindings = mode->get("binding");
  if (bindings == nullptr) {
    Fail(path, "missing required '[[mode.binding]]' entries");
  }
  const auto* list = bindings->as_array();
  if (list == nullptr || list->empty()) {
    Fail(path, "key 'binding' must be a non-empty array of tables ([[mode.binding]])");
  }
  if (list->size() != 1) {
    Fail(path, "exactly one '[[mode.binding]]' is required per EOS process; run additional "
               "robots as separate processes and ports");
  }
  std::size_t index = 0;
  for (const auto& entry : *list) {
    const auto* table = entry.as_table();
    if (table == nullptr) {
      Fail(path, "mode binding[" + std::to_string(index) + "] must be a table");
    }
    for (const auto& [key, value] : *table) {
      (void)value;
      if (key != "backend" && key != "profile") {
        Fail(path, "unknown key '" + std::string(key.str()) + "' in mode binding[" +
                       std::to_string(index) + "]");
      }
    }
    ModeBinding binding;
    const auto* backend = table->get("backend");
    const auto* profile = table->get("profile");
    if (backend == nullptr || !backend->is_string()) {
      Fail(path, "mode binding[" + std::to_string(index) + "] key 'backend' must be a string");
    }
    if (profile == nullptr || !profile->is_string()) {
      Fail(path, "mode binding[" + std::to_string(index) + "] key 'profile' must be a string");
    }
    binding.backend = backend->value<std::string>().value();
    binding.profile = profile->value<std::string>().value();
    if (binding.backend.empty() || binding.profile.empty()) {
      Fail(path,
           "mode binding[" + std::to_string(index) + "] backend and profile must be non-empty");
    }
    config.bindings.push_back(std::move(binding));
    ++index;
  }

  if (const auto* primitives = mode->get("primitives"); primitives != nullptr) {
    const auto* primitives_table = primitives->as_table();
    if (primitives_table == nullptr) {
      Fail(path, "key 'primitives' must be a table");
    }
    for (const auto& [key, value] : *primitives_table) {
      (void)value;
      if (key != "expose_operator") {
        Fail(path, "unknown key '" + std::string(key.str()) + "' in [mode.primitives]");
      }
    }
    if (const auto* expose = primitives_table->get("expose_operator"); expose != nullptr) {
      if (auto value = expose->value<bool>()) {
        config.expose_operator = *value;
      } else {
        Fail(path, "key 'expose_operator' must be a boolean");
      }
    }
  }
  return config;
}

}  // namespace eos::runtime::hardware_runtime

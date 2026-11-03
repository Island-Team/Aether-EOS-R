#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <toml++/toml.hpp>

#include "src/eos/runtime/hardware/catalog.h"
namespace eos::backends::detail {
using namespace eos::runtime::hardware_runtime;
[[noreturn]] inline void Fail(const std::string& path, const std::string& what) {
  throw std::runtime_error("binding '" + path + "': " + what);
}
inline std::string RequireString(const toml::table& tbl, const std::string& key,
                                 const std::string& path, const std::string& where) {
  const auto* node = tbl.get(key);
  if (node == nullptr) {
    Fail(path, where + ": missing required key '" + key + "'");
  }
  if (auto value = node->value<std::string>()) {
    return std::move(*value);
  }
  Fail(path, where + ": key '" + key + "' must be a string");
}

/// Reads an optional string key from `tbl`; nullopt when absent.
inline std::optional<std::string> OptionalString(const toml::table& tbl, const std::string& key,
                                                 const std::string& path,
                                                 const std::string& where) {
  const auto* node = tbl.get(key);
  if (node == nullptr) {
    return std::nullopt;
  }
  if (auto value = node->value<std::string>()) {
    return std::move(*value);
  }
  Fail(path, where + ": key '" + key + "' must be a string");
}

/// Reads a required non-negative integer key from `tbl` as uint64.
inline std::uint64_t RequireBytes(const toml::table& tbl, const std::string& key,
                                  const std::string& path, const std::string& where) {
  const auto* node = tbl.get(key);
  if (node == nullptr) {
    Fail(path, where + ": missing required key '" + key + "'");
  }
  if (auto value = node->value<std::int64_t>()) {
    if (*value >= 0) {
      return static_cast<std::uint64_t>(*value);
    }
    Fail(path, where + ": key '" + key + "' must not be negative");
  }
  Fail(path, where + ": key '" + key + "' must be an integer");
}

/// Reads a required non-negative number key from `tbl` as double.
inline double RequireDouble(const toml::table& tbl, const std::string& key, const std::string& path,
                            const std::string& where) {
  const auto* node = tbl.get(key);
  if (node == nullptr) {
    Fail(path, where + ": missing required key '" + key + "'");
  }
  if (auto value = node->value<double>()) {
    if (*value >= 0.0) {
      return *value;
    }
    Fail(path, where + ": key '" + key + "' must not be negative");
  }
  Fail(path, where + ": key '" + key + "' must be a number");
}

/// Reads a required (possibly negative) coordinate key from `tbl` as double;
/// world-frame positions are signed, unlike rates and sizes.
inline double RequireCoordinate(const toml::table& tbl, const std::string& key,
                                const std::string& path, const std::string& where) {
  const auto* node = tbl.get(key);
  if (node == nullptr) {
    Fail(path, where + ": missing required key '" + key + "'");
  }
  if (auto value = node->value<double>()) {
    return *value;
  }
  Fail(path, where + ": key '" + key + "' must be a number");
}

/// Reads an optional non-negative scalar key from `tbl`, returning `fallback`
/// when absent; fails loudly on a non-number or negative value.
inline double OptionalLength(const toml::table& tbl, const std::string& key, double fallback,
                             const std::string& path, const std::string& where) {
  const auto* node = tbl.get(key);
  if (node == nullptr) {
    return fallback;
  }
  if (auto value = node->value<double>()) {
    if (*value >= 0.0) {
      return *value;
    }
    Fail(path, where + ": key '" + key + "' must not be negative");
  }
  Fail(path, where + ": key '" + key + "' must be a number");
}

/// Reads an optional signed coordinate key from `tbl`, returning `fallback`
/// when absent; fails loudly on a non-number value.
inline double OptionalCoordinate(const toml::table& tbl, const std::string& key, double fallback,
                                 const std::string& path, const std::string& where) {
  const auto* node = tbl.get(key);
  if (node == nullptr) {
    return fallback;
  }
  if (auto value = node->value<double>()) {
    return *value;
  }
  Fail(path, where + ": key '" + key + "' must be a number");
}

/// Reads an optional non-negative integer key from `tbl` as uint64.
inline std::uint64_t OptionalBytes(const toml::table& tbl, const std::string& key,
                                   std::uint64_t fallback, const std::string& path,
                                   const std::string& where) {
  const auto* node = tbl.get(key);
  if (node == nullptr) {
    return fallback;
  }
  if (auto value = node->value<std::int64_t>()) {
    if (*value >= 0) {
      return static_cast<std::uint64_t>(*value);
    }
    Fail(path, where + ": key '" + key + "' must not be negative");
  }
  Fail(path, where + ": key '" + key + "' must be an integer");
}

/// Fails unless every key of `tbl` is in the NUL-terminated `allowed` list.
inline void RequireKnownKeys(const toml::table& tbl, const char* const* allowed,
                             const std::string& path, const std::string& where) {
  for (const auto& [key, value] : tbl) {
    (void)value;
    bool known = false;
    for (const char* const* candidate = allowed; *candidate != nullptr; ++candidate) {
      if (key == *candidate) {
        known = true;
        break;
      }
    }
    if (!known) {
      Fail(path, where + ": unknown key '" + std::string(key.str()) + "'");
    }
  }
}

inline const toml::table* OneTable(const toml::table& backend, std::string_view type, bool required,
                                   const std::string& path, const std::string& where) {
  for (const auto& [key, node] : backend) {
    if (key == "type") continue;
    if (key != type) {
      const std::string name(key.str());
      if (name == "vendor" || name == "ros2" || name == "aplay" || name == "alsa" || name == "v4l2")
        Fail(path, where + ": key '" + name + "' is only valid for backend type '" + name + "'");
      Fail(path, where + ": unknown key '" + name + "'");
    }
    if (!node.is_table()) Fail(path, where + ": key '" + std::string(type) + "' must be a table");
  }
  const auto* node = backend.get(type);
  if (!node) {
    if (required)
      Fail(path, where + ": backend type '" + std::string(type) + "' requires a [device.backend." +
                     std::string(type) + "] table");
    return nullptr;
  }
  return node->as_table();
}

inline bool Supports(std::string_view kind, std::initializer_list<std::string_view> allowed) {
  return std::find(allowed.begin(), allowed.end(), kind) != allowed.end();
}
inline bool HasPrimitive(const PrimitiveSpec& primitive,
                         std::initializer_list<std::string_view> names) {
  for (std::string_view name : names)
    if (primitive.name == name) return true;
  return false;
}
}  // namespace eos::backends::detail

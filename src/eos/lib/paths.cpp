#include "src/eos/lib/paths.h"

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace eos::lib {
namespace {

const char* GetEnv(const char* name) {
  const char* value = std::getenv(name);
  return (value != nullptr && value[0] != '\0') ? value : nullptr;
}

ConfigRootError Unresolvable(const std::string& what, const std::vector<std::string>& tried) {
  std::string message = what + ": no resolvable path; tried:";
  for (const std::string& path : tried) {
    message += "\n  " + path;
  }
  return ConfigRootError(message);
}

std::filesystem::path Absolute(const std::string& what, const std::string& value) {
  const std::filesystem::path path(value);
  if (!path.is_absolute()) {
    throw ConfigRootError(what + " must be absolute: " + value);
  }
  return path.lexically_normal();
}

std::filesystem::path ExistingRoot(const std::string& what,
                                   const std::vector<std::pair<std::string, std::string>>& values) {
  std::vector<std::string> tried;
  for (const auto& [source, value] : values) {
    if (value.empty()) {
      tried.push_back(source + " (unset)");
      continue;
    }
    const std::filesystem::path path = Absolute(what, value);
    tried.push_back(path.string());
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
      return path;
    }
    throw Unresolvable(what, tried);
  }
  throw Unresolvable(what, tried);
}

std::string EnvValue(const char* name) {
  const char* value = GetEnv(name);
  return value == nullptr ? std::string() : std::string(value);
}

std::filesystem::path MutableRoot(const char* island_env, const char* suffix,
                                  bool component_namespace) {
  const std::string explicit_root = EnvValue(island_env);
  if (!explicit_root.empty()) {
    std::filesystem::path path = Absolute(island_env, explicit_root);
    return component_namespace ? path / "eos" : path;
  }

  const std::string legacy = EnvValue("EOS_VAR");
  if (!legacy.empty()) {
    std::filesystem::path path = Absolute("EOS_VAR", legacy) / suffix;
    return component_namespace ? path / "eos" : path;
  }

  if (EnvValue("ISLAND_MODE") == "system") {
    std::filesystem::path path;
    if (std::string(suffix) == "lib") {
      path = "/var/lib/island";
    } else if (std::string(suffix) == "cache") {
      path = "/var/cache/island";
    } else if (std::string(suffix) == "log") {
      path = "/var/log/island";
    } else if (std::string(suffix) == "run") {
      path = "/run/island";
    } else {
      path = "/var/tmp/island";
    }
    return component_namespace ? path / "eos" : path;
  }

  const std::string prefix = EnvValue("ISLAND_PREFIX");
  if (prefix.empty()) {
    throw ConfigRootError(std::string(island_env) +
                          " is unset and ISLAND_PREFIX cannot provide a checkout default");
  }
  std::filesystem::path path = Absolute("ISLAND_PREFIX", prefix) / "var" / suffix;
  return component_namespace ? path / "eos" : path;
}

void CreateMutable(const std::string& what, const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::create_directories(path, ec) && ec) {
    throw Unresolvable(what + " (create failed)", {path.string() + ": " + ec.message()});
  }
}

}  // namespace

std::filesystem::path ResolveEtcRoot(const std::string& cli_arg) {
  const std::string island_etc = EnvValue("ISLAND_ETC");
  return ExistingRoot("config root", {{"--etc", cli_arg},
                                      {"EOS_ETC", EnvValue("EOS_ETC")},
                                      {"ISLAND_ETC/eos",
                                       island_etc.empty() ? std::string() : island_etc + "/eos"}});
}

std::filesystem::path ResolveContractsRoot(const std::string& cli_arg) {
  const std::string island_share = EnvValue("ISLAND_SHARE");
  return ExistingRoot("contracts root", {{"--contracts", cli_arg},
                                         {"EOS_CONTRACTS", EnvValue("EOS_CONTRACTS")},
                                         {"ISLAND_SHARE", island_share}});
}

LayoutPaths ResolveLayoutPaths(const std::string& cli_etc, const std::string& cli_contracts) {
  LayoutPaths paths{
      ResolveEtcRoot(cli_etc),
      ResolveContractsRoot(cli_contracts),
      MutableRoot("ISLAND_STATE", "lib", true),
      MutableRoot("ISLAND_CACHE", "cache", true),
      MutableRoot("ISLAND_LOG", "log", true),
      MutableRoot("ISLAND_RUN", "run", false),
      MutableRoot("ISLAND_TMP", "tmp", false),
  };
  CreateMutable("state root", paths.state);
  CreateMutable("cache root", paths.cache);
  CreateMutable("log root", paths.log);
  CreateMutable("runtime root", paths.run);
  CreateMutable("temporary root", paths.tmp);
  return paths;
}

}  // namespace eos::lib

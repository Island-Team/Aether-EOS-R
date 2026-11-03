#include "src/eos/runtime/init/stack_manager.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <fstream>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/evp.h>
#include <toml++/toml.hpp>

namespace eos::runtime::init {
namespace {

using Path = std::filesystem::path;

[[noreturn]] void Fail(const Path& path, const std::string& reason) {
  throw std::runtime_error("runtime stack '" + path.string() + "': " + reason);
}

Path Canonical(const Path& path) {
  std::error_code error;
  const Path result = std::filesystem::canonical(path, error);
  if (error) Fail(path, "cannot resolve path: " + error.message());
  return result;
}

toml::table Read(const Path& path) {
  try {
    return toml::parse_file(path.string());
  } catch (const toml::parse_error& error) {
    Fail(path, std::string(error.description()));
  }
}

void Keys(const Path& path, const toml::table& table,
          std::initializer_list<std::string_view> allowed) {
  for (const auto& [key, value] : table) {
    (void)value;
    if (std::find(allowed.begin(), allowed.end(), key.str()) == allowed.end()) {
      Fail(path, "unknown key '" + std::string(key.str()) + "'");
    }
  }
}

std::string RequiredString(const Path& path, const toml::table& table, const char* key) {
  auto value = table[key].value<std::string>();
  if (!value || value->empty()) Fail(path, std::string(key) + " must be a nonempty string");
  return *value;
}

std::vector<std::string> Strings(const Path& path, const toml::table& table, const char* key) {
  const toml::node* node = table.get(key);
  if (!node) return {};
  const toml::array* array = node->as_array();
  if (!array) Fail(path, std::string(key) + " must be a string array");
  std::vector<std::string> values;
  for (const auto& entry : *array) {
    const auto value = entry.value<std::string>();
    if (!value || value->empty()) Fail(path, std::string(key) + " must contain nonempty strings");
    values.push_back(*value);
  }
  return values;
}

const toml::array* Tables(const Path& path, const toml::table& table, const char* key) {
  const toml::node* node = table.get(key);
  if (!node) return nullptr;
  const toml::array* array = node->as_array();
  if (!array) Fail(path, std::string(key) + " must be an array of tables");
  for (const auto& entry : *array) {
    if (!entry.is_table()) Fail(path, std::string(key) + " must be an array of tables");
  }
  return array;
}

bool Enabled(const Path& path, const toml::table& table) {
  const toml::node* node = table.get("enabled");
  if (!node) return true;
  const auto enabled = node->value<bool>();
  if (!enabled) Fail(path, "enabled must be boolean");
  return *enabled;
}

struct Service {
  bool enabled{true};
  std::vector<std::string> after;
  std::vector<std::string> endpoints;
};

struct Binding {
  Path backend;
  std::string profile;
  std::vector<std::string> requires_services;
  std::vector<std::string> eos_after;
};

std::string ReleaseDigest(const Path& root) {
  if (!std::filesystem::is_regular_file(root / "release.json")) {
    Fail(root, "installed package has no release.json");
  }
  using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
  DigestContext context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
    Fail(root, "cannot initialize package digest");
  }
  std::vector<Path> entries;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    entries.push_back(entry.path());
  }
  std::sort(entries.begin(), entries.end(), [&](const Path& left, const Path& right) {
    return left.lexically_relative(root).generic_string() < right.lexically_relative(root).generic_string();
  });
  for (const Path& item : entries) {
    if (item == root / "package-receipt.json") continue;
    const auto status = std::filesystem::symlink_status(item);
    if (std::filesystem::is_symlink(status) ||
        (!std::filesystem::is_regular_file(status) && !std::filesystem::is_directory(status))) {
      Fail(item, "package tree contains unsupported entry");
    }
    const bool file = std::filesystem::is_regular_file(status);
    const auto permissions = status.permissions();
    const bool executable = file && (permissions &
        (std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
         std::filesystem::perms::others_exec)) != std::filesystem::perms::none;
    const std::string header = std::string(file ? "f " : "d ") +
        (executable ? "x " : "- ") + item.lexically_relative(root).generic_string() + "\n";
    if (EVP_DigestUpdate(context.get(), header.data(), header.size()) != 1) {
      Fail(item, "cannot hash package entry");
    }
    if (!file) continue;
    std::ifstream input(item, std::ios::binary);
    if (!input) Fail(item, "cannot read package entry");
    char block[1024 * 1024];
    while (input.read(block, sizeof(block)) || input.gcount() > 0) {
      if (EVP_DigestUpdate(context.get(), block, static_cast<std::size_t>(input.gcount())) != 1) {
        Fail(item, "cannot hash package entry");
      }
    }
    if (!input.eof()) Fail(item, "cannot finish reading package entry");
  }
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int size = 0;
  if (EVP_DigestFinal_ex(context.get(), digest, &size) != 1 || size != 32) {
    Fail(root, "cannot finalize package digest");
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result = "sha256:";
  for (unsigned int i = 0; i < size; ++i) {
    result.push_back(kHex[digest[i] >> 4]);
    result.push_back(kHex[digest[i] & 15]);
  }
  return result;
}

void Services(const Path& path, const toml::table& table, std::map<std::string, Service>* services) {
  const toml::array* entries = Tables(path, table, "service");
  if (!entries) return;
  for (const auto& entry : *entries) {
    const auto& item = *entry.as_table();
    Keys(path, item, {"name", "role", "command", "after", "ready_tcp", "ready_unix",
                      "timeout_ms", "env", "enabled"});
    const std::string name = RequiredString(path, item, "name");
    Service service;
    service.enabled = Enabled(path, item);
    service.after = Strings(path, item, "after");
    if (const auto* tcp = item.get("ready_tcp")) {
      auto value = tcp->value<std::string>();
      if (!value || value->empty()) Fail(path, name + ".ready_tcp must be nonempty");
      service.endpoints.push_back("tcp:" + *value);
    }
    if (const auto* unix = item.get("ready_unix")) {
      auto value = unix->value<std::string>();
      if (!value || value->empty()) Fail(path, name + ".ready_unix must be nonempty");
      service.endpoints.push_back("unix:" + *value);
    }
    if (service.endpoints.size() > 1) Fail(path, name + " has multiple readiness endpoints");
    if (!services->emplace(name, std::move(service)).second) {
      Fail(path, "duplicate service '" + name + "'");
    }
  }
}

void Bindings(const Path& path, const Path& eos_config, const toml::table& table,
              std::vector<Binding>* bindings) {
  const toml::array* entries = Tables(path, table, "binding");
  if (!entries) return;
  for (const auto& entry : *entries) {
    const auto& item = *entry.as_table();
    Keys(path, item, {"backend", "profile", "requires", "eos_after"});
    Path backend = RequiredString(path, item, "backend");
    if (!backend.is_absolute()) backend = eos_config.parent_path() / backend;
    bindings->push_back({Canonical(backend), RequiredString(path, item, "profile"),
                         Strings(path, item, "requires"), Strings(path, item, "eos_after")});
  }
}

void SkillCapabilities(const Path& path, const toml::table& table,
                       RuntimeStack* stack,
                       std::set<std::tuple<std::string, std::string, std::string>>* identities) {
  const toml::array* entries = Tables(path, table, "skill_capability");
  if (!entries) return;
  for (const auto& entry : *entries) {
    const auto& item = *entry.as_table();
    Keys(path, item, {"skill", "skill_version", "slot", "argument_pointer",
                      "robot_id", "device_class", "primitive", "primitive_version"});
    SkillCapability capability{
        RequiredString(path, item, "skill"),
        RequiredString(path, item, "skill_version"),
        RequiredString(path, item, "slot"),
        RequiredString(path, item, "argument_pointer"),
        RequiredString(path, item, "robot_id"),
        RequiredString(path, item, "device_class"),
        RequiredString(path, item, "primitive"),
        RequiredString(path, item, "primitive_version")};
    try {
      (void)nlohmann::json::json_pointer(capability.argument_pointer);
    } catch (const std::exception&) {
      Fail(path, "skill capability argument_pointer is not a JSON pointer");
    }
    if (!identities->emplace(capability.skill, capability.skill_version,
                             capability.slot).second)
      Fail(path, "duplicate skill capability slot '" + capability.slot + "'");
    stack->skill_capabilities.push_back(std::move(capability));
  }
}

void VerifyPackageSelection(const Path& module_path, const toml::table& metadata,
                            const Path& package_root) {
  const bool has_package = metadata.contains("package");
  const bool has_version = metadata.contains("version");
  if (has_package != has_version) Fail(module_path, "module package and version must be paired");
  if (!has_package) return;
  const std::string package = RequiredString(module_path, metadata, "package");
  const std::string version = RequiredString(module_path, metadata, "version");
  static const std::regex package_id("^[a-z][a-z0-9-]*/[a-z][a-z0-9-]*$");
  static const std::regex package_version("^[0-9]+\\.[0-9]+\\.[0-9]+([-+][a-zA-Z0-9.-]+)?$");
  if (!std::regex_match(package, package_id) || !std::regex_match(version, package_version)) {
    Fail(module_path, "invalid package identity or version");
  }
  std::string expected_module = package;
  expected_module.replace(expected_module.find('/'), 1, "--");
  if (expected_module != module_path.parent_path().filename()) {
    Fail(module_path, "module directory does not match package identity");
  }
  const Path lock_path = package_root / "isos.lock";
  const toml::table lock = Read(lock_path);
  Keys(lock_path, lock, {"version", "package"});
  if (lock["version"].value<std::int64_t>() != 1) Fail(lock_path, "expected version = 1");
  const toml::array* entries = Tables(lock_path, lock, "package");
  if (!entries) Fail(lock_path, "missing package selections");
  bool selected = false;
  std::string selected_digest;
  std::set<std::string> ids;
  for (const auto& entry : *entries) {
    const auto& item = *entry.as_table();
    const std::string id = RequiredString(lock_path, item, "id");
    if (!ids.insert(id).second) Fail(lock_path, "duplicate package selection '" + id + "'");
    if (id == package) {
      selected = RequiredString(lock_path, item, "version") == version;
      selected_digest = RequiredString(lock_path, item, "digest");
    }
  }
  if (!selected) Fail(module_path, "package " + package + "@" + version + " is not selected in isos.lock");
  const Path installed = package_root / "installed" / package / version;
  if (ReleaseDigest(installed) != selected_digest) {
    Fail(module_path, "installed release digest differs from isos.lock for " + package + "@" + version);
  }
}

}  // namespace

RuntimeStack ValidateRuntimeStack(const Path& stack_path, const Path& eos_config_path,
                                  const hardware_runtime::ModeConfig& mode) {
  RuntimeStack effective;
  std::set<std::tuple<std::string, std::string, std::string>> skill_slots;
  const Path stack = Canonical(stack_path);
  const Path config = Canonical(eos_config_path);
  const toml::table root = Read(stack);
  Keys(stack, root, {"version", "eos_config", "modules", "package_root", "service", "binding"});
  if (root["version"].value<std::int64_t>() != 1) Fail(stack, "expected version = 1");
  Path declared_config = RequiredString(stack, root, "eos_config");
  if (!declared_config.is_absolute()) declared_config = stack.parent_path() / declared_config;
  if (Canonical(declared_config) != config) Fail(stack, "eos_config differs from selected EOS config");
  Path package_root;
  if (root.contains("package_root")) {
    package_root = RequiredString(stack, root, "package_root");
    if (!package_root.is_absolute()) package_root = stack.parent_path() / package_root;
  } else {
    const char* xdg_data_home = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    if (xdg_data_home && *xdg_data_home) package_root = Path(xdg_data_home) / "island/packages";
    else if (home && *home) package_root = Path(home) / ".local/share/island/packages";
  }

  std::map<std::string, Service> services;
  std::vector<Binding> bindings;
  Services(stack, root, &services);
  Bindings(stack, config, root, &bindings);
  std::set<Path> module_paths;
  for (const std::string& reference : Strings(stack, root, "modules")) {
    const Path module_path = Canonical(stack.parent_path() / reference);
    if (!module_paths.insert(module_path).second) Fail(stack, "duplicate module reference");
    if (module_path.filename() != "module.toml") Fail(module_path, "expected module.toml");
    const toml::table module = Read(module_path);
    Keys(module_path, module, {"version", "module", "service", "binding", "skill_capability", "skill_directory"});
    if (module["version"].value<std::int64_t>() != 1) Fail(module_path, "expected version = 1");
    const toml::table* metadata = module["module"].as_table();
    if (!metadata) Fail(module_path, "missing [module]");
    Keys(module_path, *metadata, {"id", "enabled", "package", "version"});
    if (RequiredString(module_path, *metadata, "id") != module_path.parent_path().filename()) {
      Fail(module_path, "module id must match its directory name");
    }
    if (!Enabled(module_path, *metadata)) continue;
    VerifyPackageSelection(module_path, *metadata, package_root);
    if (module.contains("skill_directory")) {
      if (!metadata->contains("package"))
        Fail(module_path, "skill_directory requires a locked package selection");
      const Path relative = RequiredString(module_path, module, "skill_directory");
      if (relative.is_absolute() || relative.empty())
        Fail(module_path, "skill_directory must be relative to the installed release");
      const Path release = Canonical(package_root / "installed" /
          RequiredString(module_path, *metadata, "package") /
          RequiredString(module_path, *metadata, "version"));
      const Path directory = Canonical(release / relative);
      const Path inside = directory.lexically_relative(release);
      if (inside.empty() || *inside.begin() == ".." ||
          !std::filesystem::is_directory(directory))
        Fail(module_path, "skill_directory escapes or is absent from installed release");
      effective.skill_providers.push_back({RequiredString(module_path, *metadata, "id"),
                                           directory});
    }
    Services(module_path, module, &services);
    Bindings(module_path, config, module, &bindings);
    SkillCapabilities(module_path, module, &effective, &skill_slots);
  }

  std::set<std::pair<Path, std::string>> declared;
  for (const Binding& binding : bindings) {
    if (!declared.emplace(binding.backend, binding.profile).second) {
      Fail(stack, "duplicate binding declaration for " + binding.backend.string() + " profile " + binding.profile);
    }
  }
  std::vector<std::string> eos_after;
  for (const auto& selected : mode.bindings) {
    Path backend = selected.backend;
    if (!backend.is_absolute()) backend = config.parent_path() / backend;
    const Path canonical_backend = Canonical(backend);
    const auto match = std::find_if(bindings.begin(), bindings.end(), [&](const Binding& binding) {
      return binding.backend == canonical_backend && binding.profile == selected.profile;
    });
    if (match == bindings.end()) {
      Fail(stack, "no declaration for selected binding " + canonical_backend.string() +
                      " profile " + selected.profile);
    }
    for (const auto& name : match->requires_services) {
      auto service = services.find(name);
      if (service == services.end() || !service->second.enabled) {
        Fail(stack, "binding requires enabled service '" + name + "'");
      }
    }
    for (const auto& name : match->eos_after) {
      auto service = services.find(name);
      if (service == services.end() || !service->second.enabled) {
        Fail(stack, "EOS startup depends on missing or disabled service '" + name + "'");
      }
      eos_after.push_back(name);
    }
  }
  if (services.find("eosd") == services.end() || !services.at("eosd").enabled) {
    Fail(stack, "eosd must be an enabled service");
  }
  std::set<std::string> endpoints;
  for (const auto& [name, service] : services) {
    if (!service.enabled) continue;
    for (const auto& dependency : service.after) {
      auto target = services.find(dependency);
      if (target == services.end() || !target->second.enabled) {
        Fail(stack, "service '" + name + "' depends on missing or disabled '" + dependency + "'");
      }
    }
    for (const auto& endpoint : service.endpoints) {
      if (!endpoints.insert(endpoint).second) Fail(stack, "duplicate readiness endpoint '" + endpoint + "'");
    }
  }
  for (const auto& name : eos_after) services.at("eosd").after.push_back(name);
  std::map<std::string, int> visit_state;
  std::function<void(const std::string&)> visit = [&](const std::string& name) {
    if (visit_state[name] == 1) Fail(stack, "service dependency cycle at '" + name + "'");
    if (visit_state[name] == 2) return;
    visit_state[name] = 1;
    for (const auto& dependency : services.at(name).after) visit(dependency);
    visit_state[name] = 2;
  };
  for (const auto& [name, service] : services) {
    if (service.enabled) visit(name);
  }
  return effective;
}

}  // namespace eos::runtime::init

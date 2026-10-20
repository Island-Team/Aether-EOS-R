#include <dlfcn.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <nlohmann/json-schema.hpp>
#include <regex>
#include <set>
#include <stdexcept>
#include <tuple>

#include "src/eos/lib/log.h"
#include "src/eos/runtime/skills/core/admission.h"
#include "src/eos/runtime/skills/manager/logging.h"
#include "src/eos/runtime/skills/native/detail.h"

namespace eos::runtime::skills::native {
using namespace detail;
namespace {

nlohmann::json ParseSchema(eos_skill_text_v2 text, const std::string& field,
                           std::unique_ptr<nlohmann::json_schema::json_validator>* validator) {
  const std::string encoded = CopyText(text, kMaxJsonBytes, field);
  nlohmann::json schema;
  try {
    schema = nlohmann::json::parse(encoded);
  } catch (const std::exception& error) {
    throw std::runtime_error(field + " is not valid JSON: " + error.what());
  }
  if (!schema.is_object()) throw std::runtime_error(field + " must be a JSON object");
  const auto no_external_references = [](const nlohmann::json_uri& uri, nlohmann::json&) {
    throw std::invalid_argument("skill schemas must be self-contained; unresolved reference '" +
                                uri.url() + "'");
  };
  auto compiled = std::make_unique<nlohmann::json_schema::json_validator>(
      no_external_references, nlohmann::json_schema::default_string_format_check);
  try {
    compiled->set_root_schema(schema);
  } catch (const std::exception& error) {
    throw std::runtime_error(field + " does not compile: " + error.what());
  }
  *validator = std::move(compiled);
  return schema;
}

bool ValidSignalName(const std::string& value) {
  static const std::regex pattern("^[a-z][a-z0-9]*(?:\\.[a-z][a-z0-9]*)+$");
  return std::regex_match(value, pattern) && value != "cancel";
}

}  // namespace

Runtime Runtime::LoadDirectory(const std::filesystem::path& directory,
                               dispatch::Dispatcher& dispatcher, ResourceReader resource_reader,
                               ResourceTemplateValidator resource_template_validator,
                               InferenceRunner inference_runner) {
  auto& logger = SkillLogger();
  if (!directory.is_absolute()) {
    eos::lib::LogEvent(logger, spdlog::level::warn, "skills_load_rejected",
                       {{"reason", "directory_not_absolute"}});
    throw std::runtime_error("skill directory must be absolute: " + directory.string());
  }
  std::error_code ec;
  if (!std::filesystem::is_directory(directory, ec)) {
    eos::lib::LogEvent(logger, spdlog::level::warn, "skills_load_rejected",
                       {{"directory", directory.string()}, {"reason", "directory_not_readable"}});
    throw std::runtime_error("skill directory is not readable: " + directory.string());
  }
  eos::lib::LogEvent(logger, spdlog::level::info, "skills_load_started",
                     {{"directory", directory.string()}});
  try {
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
      const std::filesystem::path path = entry.path();
      const std::string name = path.filename().string();
      if (name.size() >= 9 && name.compare(name.size() - 9, 9, ".skill.so") == 0)
        paths.push_back(path);
    }
    std::sort(paths.begin(), paths.end());
    if (paths.size() > kMaxLibraries)
      throw std::runtime_error("skill directory has too many libraries");

    auto impl = std::make_unique<Impl>();
    std::set<std::pair<std::string, std::string>> identities;
    std::set<std::pair<std::string, std::string>> known;
    for (const auto& primitive : dispatcher.PrimitiveIdentities())
      known.emplace(primitive.name, primitive.version);
    for (const std::filesystem::path& path : paths) {
      const std::filesystem::file_status status = std::filesystem::symlink_status(path, ec);
      if (ec || status.type() != std::filesystem::file_type::regular)
        throw std::runtime_error("skill library must be a regular non-symlink file: " +
                                 path.string());
      dlerror();
      void* raw_handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
      const std::string load_error = raw_handle == nullptr ? DlError() : std::string();
      if (raw_handle == nullptr)
        throw std::runtime_error("cannot load skill library '" + path.string() +
                                 "': " + load_error);
      auto handle = std::make_unique<Handle>();
      handle->value = raw_handle;
      handle->path = path.string();

      const eos_skill_library_v2* library_v2 = nullptr;
      dlerror();
      void* symbol_v2 = dlsym(raw_handle, EOS_SKILL_QUERY_SYMBOL_V2);
      const char* v2_error = dlerror();
      try {
        if (v2_error != nullptr || symbol_v2 == nullptr)
          throw std::runtime_error("skill library '" + path.string() +
                                   "' has no ABI v2 query symbol");
        eos_skill_library_query_v2_fn query_v2 = nullptr;
        static_assert(sizeof(query_v2) == sizeof(symbol_v2));
        std::memcpy(&query_v2, &symbol_v2, sizeof(query_v2));
        library_v2 = query_v2(EOS_SKILL_ABI_VERSION_V2);
      } catch (const std::runtime_error&) {
        throw;
      } catch (...) {
        throw std::runtime_error("skill library query threw an exception: " + path.string());
      }
      if (library_v2 == nullptr || library_v2->abi_version != EOS_SKILL_ABI_VERSION_V2 ||
          library_v2->struct_size < EOS_SKILL_LIBRARY_V2_BASE_SIZE)
        throw std::runtime_error("skill library has an incompatible ABI: " + path.string());
      const eos_skill_text_v2 library_name_text = library_v2->name;
      const eos_skill_text_v2 library_version_text = library_v2->version;
      const eos_skill_text_v2 build_id_text = library_v2->build_id;
      const std::uint32_t skill_count = library_v2->skill_count;
      const std::string library_name = CopyText(library_name_text, kMaxNameBytes, "library name");
      const std::string library_version =
          CopyText(library_version_text, kMaxVersionBytes, "library version");
      const std::string build_id =
          CopyText(build_id_text, kMaxVersionBytes, "library build_id", false);
      if (!ValidName(library_name) || !ValidVersion(library_version))
        throw std::runtime_error("skill library has an invalid name or version: " + path.string());
      if (skill_count == 0 || skill_count > kMaxSkillsPerLibrary || library_v2->skills == nullptr)
        throw std::runtime_error("skill library has an invalid skill table: " + path.string());
      const std::uint32_t descriptor_size =
          EOS_SKILL_LIBRARY_V2_HAS(library_v2, skill_descriptor_size)
              ? library_v2->skill_descriptor_size
              : EOS_SKILL_DESCRIPTOR_V2_BASE_SIZE;
      if (descriptor_size < EOS_SKILL_DESCRIPTOR_V2_BASE_SIZE ||
          descriptor_size % alignof(eos_skill_descriptor_v2) != 0)
        throw std::runtime_error("skill library has an invalid descriptor stride: " +
                                 path.string());
      for (std::uint32_t i = 0; i < skill_count; ++i) {
        const auto* descriptor_bytes = reinterpret_cast<const unsigned char*>(library_v2->skills);
        const auto* descriptor_v2 = reinterpret_cast<const eos_skill_descriptor_v2*>(
            descriptor_bytes + static_cast<std::size_t>(i) * descriptor_size);
        if (descriptor_v2->struct_size < EOS_SKILL_DESCRIPTOR_V2_BASE_SIZE ||
            descriptor_v2->struct_size > descriptor_size ||
            descriptor_v2->execute == nullptr)
          throw std::runtime_error("skill descriptor is incomplete in " + path.string());
        const eos_skill_text_v2 name = descriptor_v2->name;
        const eos_skill_text_v2 version = descriptor_v2->version;
        const eos_skill_text_v2 description = descriptor_v2->description;
        const eos_skill_text_v2 parameters_schema = descriptor_v2->parameters_schema_json;
        const eos_skill_text_v2 returns_schema = descriptor_v2->returns_schema_json;
        const std::uint32_t primitive_count = descriptor_v2->primitive_count;
        const eos_skill_primitive_reference_v2* primitives = descriptor_v2->primitives;
        const std::uint32_t resource_count = descriptor_v2->resource_count;
        const eos_skill_resource_v2* resources = descriptor_v2->resources;
        LoadedSkill loaded;
        loaded.metadata.name = CopyText(name, kMaxNameBytes, "skill name");
        loaded.metadata.version = CopyText(version, kMaxVersionBytes, "skill version");
        loaded.metadata.description =
            CopyText(description, kMaxDescriptionBytes, "skill description", false);
        if (!ValidName(loaded.metadata.name) || !ValidVersion(loaded.metadata.version))
          throw std::runtime_error("skill has an invalid name or version in " + path.string());
        if (!identities.insert({loaded.metadata.name, loaded.metadata.version}).second)
          throw std::runtime_error("duplicate skill '" + loaded.metadata.name + "' version '" +
                                   loaded.metadata.version + "'");
        loaded.metadata.parameters_schema = ParseSchema(
            parameters_schema, "skill '" + loaded.metadata.name + "' parameters_schema_json",
            &loaded.parameters_validator);
        loaded.metadata.returns_schema =
            ParseSchema(returns_schema, "skill '" + loaded.metadata.name + "' returns_schema_json",
                        &loaded.returns_validator);
        if (primitive_count > kMaxPrimitivesPerSkill ||
            (primitive_count != 0 && primitives == nullptr))
          throw std::runtime_error("skill '" + loaded.metadata.name +
                                   "' has an invalid primitive allowlist");
        std::set<std::pair<std::string, std::string>> declared;
        for (std::uint32_t primitive = 0; primitive < primitive_count; ++primitive) {
          const eos_skill_primitive_reference_v2& reference = primitives[primitive];
          if (reference.struct_size != sizeof(eos_skill_primitive_reference_v2))
            throw std::runtime_error("skill '" + loaded.metadata.name +
                                     "' has an incompatible primitive reference");
          const std::string name = CopyText(reference.name, kMaxNameBytes, "primitive name");
          const std::string version =
              CopyText(reference.version, kMaxVersionBytes, "primitive version");
          if (!ValidName(name) || !ValidVersion(version))
            throw std::runtime_error("skill '" + loaded.metadata.name +
                                     "' has an invalid primitive reference");
          if (known.count({name, version}) == 0)
            throw std::runtime_error("skill '" + loaded.metadata.name +
                                     "' declares unknown primitive '" + name + "' version '" +
                                     version + "'");
          if (!declared.insert({name, version}).second)
            throw std::runtime_error("skill '" + loaded.metadata.name +
                                     "' declares duplicate primitive '" + name + "' version '" +
                                     version + "'");
          loaded.metadata.primitives.push_back({name, version});
        }
        if (resource_count > kMaxResourcesPerSkill || (resource_count != 0 && resources == nullptr))
          throw std::runtime_error("skill '" + loaded.metadata.name +
                                   "' has invalid resource metadata");
        std::set<std::string> resource_templates;
        for (std::uint32_t resource = 0; resource < resource_count; ++resource) {
          const eos_skill_resource_v2& declared_resource = resources[resource];
          if (declared_resource.struct_size != sizeof(eos_skill_resource_v2))
            throw std::runtime_error("skill '" + loaded.metadata.name +
                                     "' has incompatible resource metadata");
          Resource copied{CopyText(declared_resource.uri_template, kMaxResourceTemplateBytes,
                                   "resource uri_template"),
                          CopyText(declared_resource.description, kMaxDescriptionBytes,
                                   "resource description", false)};
          if (!ValidResourceTemplate(copied.uri_template))
            throw std::runtime_error("skill '" + loaded.metadata.name +
                                     "' has an invalid resource URI template");
          if (resource_template_validator && !resource_template_validator(copied.uri_template)) {
            throw std::runtime_error("skill '" + loaded.metadata.name +
                                     "' declares an unreadable resource URI template '" +
                                     copied.uri_template + "'");
          }
          if (!resource_templates.insert(copied.uri_template).second)
            throw std::runtime_error("skill '" + loaded.metadata.name +
                                     "' declares a duplicate resource URI template");
          loaded.metadata.resources.push_back(std::move(copied));
        }
        if (descriptor_v2->reentrant > 1)
          throw std::runtime_error("skill '" + loaded.metadata.name +
                                   "' has an invalid reentrant declaration");
        loaded.metadata.supports_signals = true;
        loaded.metadata.reentrant = descriptor_v2->reentrant == 1;
        if (descriptor_v2->signal_count > kMaxSignalsPerSkill ||
            (descriptor_v2->signal_count != 0 && descriptor_v2->signals == nullptr))
          throw std::runtime_error("skill '" + loaded.metadata.name +
                                   "' has invalid SIGNAL metadata");
        std::set<std::pair<std::uint32_t, std::string>> signals;
        for (std::uint32_t signal = 0; signal < descriptor_v2->signal_count; ++signal) {
          const eos_skill_signal_descriptor_v2& declared = descriptor_v2->signals[signal];
          if (declared.struct_size != sizeof(eos_skill_signal_descriptor_v2))
            throw std::runtime_error("skill '" + loaded.metadata.name +
                                     "' has incompatible SIGNAL metadata");
          const std::string signal_name = CopyText(declared.name, kMaxNameBytes, "SIGNAL name");
          if (!ValidSignalName(signal_name) || (declared.direction != EOS_SKILL_SIGNAL_ACCEPTED &&
                                                declared.direction != EOS_SKILL_SIGNAL_EMITTED))
            throw std::runtime_error("skill '" + loaded.metadata.name +
                                     "' has an invalid SIGNAL declaration");
          if (!signals.emplace(declared.direction, signal_name).second)
            throw std::runtime_error("skill '" + loaded.metadata.name +
                                     "' declares a duplicate SIGNAL '" + signal_name + "'");
          std::unique_ptr<nlohmann::json_schema::json_validator> validator;
          nlohmann::json schema = ParseSchema(
              declared.schema_json,
              "skill '" + loaded.metadata.name + "' SIGNAL '" + signal_name + "' schema",
              &validator);
          const auto type = schema.find("type");
          if (type == schema.end() || !type->is_string() || *type != "object")
            throw std::runtime_error("skill '" + loaded.metadata.name + "' SIGNAL '" + signal_name +
                                     "' schema must declare object type");
          SignalDescriptor copied{signal_name, std::move(schema)};
          if (declared.direction == EOS_SKILL_SIGNAL_ACCEPTED) {
            loaded.metadata.accepted_signals.push_back(std::move(copied));
            loaded.accepted_signal_validators.emplace(signal_name, std::move(validator));
          } else {
            loaded.metadata.emitted_signals.push_back(std::move(copied));
            loaded.emitted_signal_validators.emplace(signal_name, std::move(validator));
          }
        }
        if (descriptor_v2->claim_count > kMaxClaimsPerSkill ||
            (descriptor_v2->claim_count != 0 && descriptor_v2->claims == nullptr))
          throw std::runtime_error("skill '" + loaded.metadata.name +
                                   "' has invalid claim metadata");
        std::set<std::tuple<std::uint32_t, std::uint32_t, std::string>> claims;
        for (std::uint32_t claim = 0; claim < descriptor_v2->claim_count; ++claim) {
          const eos_skill_claim_template_v2& declared = descriptor_v2->claims[claim];
          if (declared.struct_size != sizeof(eos_skill_claim_template_v2) ||
              (declared.kind != EOS_SKILL_CLAIM_DEVICE &&
               declared.kind != EOS_SKILL_CLAIM_RESOURCE) ||
              (declared.mode != EOS_SKILL_CLAIM_SHARED_READ &&
               declared.mode != EOS_SKILL_CLAIM_EXCLUSIVE_USE))
            throw std::runtime_error("skill '" + loaded.metadata.name +
                                     "' has an invalid claim declaration");
          const std::string pointer =
              CopyText(declared.argument_pointer, kMaxJsonBytes, "claim argument_pointer");
          try {
            nlohmann::json::json_pointer parsed(pointer);
            (void)parsed;
          } catch (const std::exception&) {
            throw std::runtime_error("skill '" + loaded.metadata.name +
                                     "' has an invalid claim JSON pointer");
          }
          if (!claims.insert({declared.kind, declared.mode, pointer}).second)
            throw std::runtime_error("skill '" + loaded.metadata.name +
                                     "' declares a duplicate claim template");
          loaded.metadata.claim_templates.push_back(
              {declared.kind == EOS_SKILL_CLAIM_DEVICE ? ClaimKind::kDevice : ClaimKind::kResource,
               declared.mode == EOS_SKILL_CLAIM_SHARED_READ ? ClaimMode::kSharedRead
                                                            : ClaimMode::kExclusiveUse,
               pointer});
        }
        if (EOS_SKILL_DESCRIPTOR_V2_HAS(descriptor_v2, sources)) {
          if (descriptor_v2->source_count > kMaxSourcesPerSkill ||
              (descriptor_v2->source_count != 0 && descriptor_v2->sources == nullptr))
            throw std::runtime_error("skill '" + loaded.metadata.name +
                                     "' has invalid source metadata");
          std::set<std::pair<std::uint32_t, std::string>> sources;
          for (std::uint32_t index = 0; index < descriptor_v2->source_count; ++index) {
            const auto& declared = descriptor_v2->sources[index];
            if (declared.struct_size != sizeof(declared) ||
                (declared.authority != EOS_SKILL_SOURCE_READ &&
                 declared.authority != EOS_SKILL_SOURCE_WRITE))
              throw std::runtime_error("skill has an invalid source declaration");
            const std::string topic = CopyText(declared.topic, kMaxNameBytes, "source topic");
            if (!ValidName(topic) || !sources.emplace(declared.authority, topic).second)
              throw std::runtime_error("skill has an invalid or duplicate source topic");
            std::unique_ptr<nlohmann::json_schema::json_validator> validator;
            nlohmann::json schema = ParseSchema(declared.schema_json, "source schema", &validator);
            if (schema.value("type", std::string{}) != "object")
              throw std::runtime_error("source schema must declare object type");
            const SourceAuthority authority = declared.authority == EOS_SKILL_SOURCE_READ
                                                  ? SourceAuthority::kRead
                                                  : SourceAuthority::kWrite;
            loaded.metadata.sources.push_back({topic, std::move(schema), authority});
            loaded.source_validators.emplace(std::make_pair(authority, topic),
                                             std::move(validator));
          }
        }
        if (EOS_SKILL_DESCRIPTOR_V2_HAS(descriptor_v2, inferences)) {
          if (descriptor_v2->inference_count > kMaxSourcesPerSkill ||
              (descriptor_v2->inference_count != 0 && descriptor_v2->inferences == nullptr))
            throw std::runtime_error("skill has invalid inference metadata");
          std::set<std::uint32_t> inferences;
          for (std::uint32_t index = 0; index < descriptor_v2->inference_count; ++index) {
            const auto& declared = descriptor_v2->inferences[index];
            if (declared.struct_size != sizeof(declared) ||
                declared.authority < EOS_SKILL_INFERENCE_VAD ||
                declared.authority > EOS_SKILL_INFERENCE_VLM ||
                !inferences.insert(declared.authority).second)
              throw std::runtime_error("skill has an invalid or duplicate inference authority");
            loaded.metadata.inference_authorities.push_back(
                static_cast<InferenceAuthority>(declared.authority - 1));
          }
        }
        loaded.metadata.library = library_name;
        loaded.metadata.library_version = library_version;
        loaded.metadata.build_id = build_id;
        loaded.descriptor_v2 = descriptor_v2;
        impl->skills.push_back(std::move(loaded));
      }
      impl->handles.push_back(std::move(handle));
      eos::lib::LogEvent(logger, spdlog::level::info, "skills_library_loaded",
                         {{"library", library_name},
                          {"version", library_version},
                          {"skills", skill_count},
                          {"abi_version", 2}});
    }
    for (const auto& handle : impl->handles) handle->process_lifetime = true;
    std::sort(impl->skills.begin(), impl->skills.end(),
              [](const LoadedSkill& left, const LoadedSkill& right) {
                return std::tie(left.metadata.name, left.metadata.version) <
                       std::tie(right.metadata.name, right.metadata.version);
              });
    for (auto& skill : impl->skills)
      impl->skills_by_identity.emplace(
          std::make_pair(skill.metadata.name, skill.metadata.version), &skill);
    eos::lib::LogEvent(logger, spdlog::level::info, "skills_load_completed",
                       {{"directory", directory.string()},
                        {"libraries", impl->handles.size()},
                        {"skills", impl->skills.size()}});
    return Runtime(std::move(impl), dispatcher, std::move(resource_reader),
                   std::move(inference_runner));
  } catch (const std::exception& error) {
    eos::lib::LogEvent(logger, spdlog::level::err, "skills_load_failed",
                       {{"directory", directory.string()},
                        {"reason", "library_error"},
                        {"message", error.what()}});
    throw;
  } catch (...) {
    eos::lib::LogEvent(logger, spdlog::level::err, "skills_load_failed",
                       {{"directory", directory.string()}, {"reason", "unknown_exception"}});
    throw;
  }
}
}  // namespace eos::runtime::skills::native

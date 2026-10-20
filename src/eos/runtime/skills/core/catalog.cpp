#include "src/eos/runtime/skills/core/catalog.h"

#include "src/eos/runtime/skills/core/admission.h"

#include <nlohmann/json-schema.hpp>
#include <regex>
#include <set>
#include <stdexcept>
#include <tuple>
namespace eos::runtime::skills {
namespace {
bool ValidName(const std::string& value) {
  static const std::regex pattern("^[a-z][a-z0-9]*(\\.[a-z][a-z0-9_-]*)+$");
  return std::regex_match(value, pattern);
}
void ValidateDescriptor(const SignalDescriptor& signal) {
  if (!ValidName(signal.name) || !signal.schema.is_object() ||
      signal.schema.value("type", std::string{}) != "object")
    throw std::invalid_argument("invalid skill SIGNAL declaration");
  nlohmann::json_schema::json_validator validator(
      nullptr, nlohmann::json_schema::default_string_format_check);
  validator.set_root_schema(signal.schema);
}
void ValidateSchema(const nlohmann::json& schema, const char* field) {
  if (!schema.is_object()) throw std::invalid_argument(std::string(field) + " must be an object");
  nlohmann::json_schema::json_validator validator(
      nullptr, nlohmann::json_schema::default_string_format_check);
  validator.set_root_schema(schema);
}
}  // namespace
Catalog::Catalog(Provider& provider) : skills_(provider.Discover()) {
  std::set<std::pair<std::string, std::string>> identities;
  for (const auto& skill : skills_) {
    if (!ValidMetadataName(skill.name) || !ValidMetadataVersion(skill.version))
      throw std::invalid_argument("invalid skill identity");
    if (!identities.emplace(skill.name, skill.version).second)
      throw std::invalid_argument("duplicate admitted skill");
    ValidateSchema(skill.parameters_schema, "parameters schema");
    ValidateSchema(skill.returns_schema, "returns schema");
    if (skill.execution_state_schema) {
      ValidateSchema(*skill.execution_state_schema, "execution state schema");
      if (skill.execution_state_schema->value("type", std::string{}) != "object")
        throw std::invalid_argument("execution state schema must describe an object");
    }
    if (skill.related_process_state_schema) {
      ValidateSchema(*skill.related_process_state_schema, "related process state schema");
      if (skill.related_process_state_schema->value("type", std::string{}) != "object")
        throw std::invalid_argument("related process state schema must describe an object");
    }
    std::set<std::pair<std::string, std::string>> primitives;
    for (const auto& primitive : skill.primitives) {
      if (!ValidMetadataName(primitive.name) || !ValidMetadataVersion(primitive.version))
        throw std::invalid_argument("invalid primitive reference");
      if (!primitives.emplace(primitive.name, primitive.version).second)
        throw std::invalid_argument("duplicate primitive reference");
    }
    std::set<std::string> resources;
    for (const auto& resource : skill.resources) {
      if (!ValidResourceTemplate(resource.uri_template))
        throw std::invalid_argument("invalid resource URI template");
      if (!resources.insert(resource.uri_template).second)
        throw std::invalid_argument("duplicate resource URI template");
    }
    std::set<std::string> accepted_names;
    for (const auto& signal : skill.accepted_signals) {
      ValidateDescriptor(signal);
      if (!accepted_names.insert(signal.name).second)
        throw std::invalid_argument("duplicate SIGNAL declaration");
    }
    std::set<std::string> emitted_names;
    for (const auto& signal : skill.emitted_signals) {
      ValidateDescriptor(signal);
      if (!emitted_names.insert(signal.name).second)
        throw std::invalid_argument("duplicate SIGNAL declaration");
    }
    std::set<std::pair<std::string, SourceAuthority>> source_authorities;
    for (const auto& source : skill.sources) {
      if (!ValidName(source.topic) || !source.schema.is_object() ||
          source.schema.value("type", std::string{}) != "object")
        throw std::invalid_argument("invalid skill source declaration");
      nlohmann::json_schema::json_validator validator(
          nullptr, nlohmann::json_schema::default_string_format_check);
      validator.set_root_schema(source.schema);
      if (!source_authorities.emplace(source.topic, source.authority).second)
        throw std::invalid_argument("duplicate skill source declaration");
    }
    std::set<InferenceAuthority> inference_authorities;
    for (const auto authority : skill.inference_authorities) {
      if (!inference_authorities.insert(authority).second)
        throw std::invalid_argument("duplicate skill inference authority");
    }
    std::set<std::string> ipc_channels;
    for (const auto& channel : skill.ipc_channels) {
      if (!ValidName(channel.name) || (!channel.can_send && !channel.can_receive) ||
          !channel.schema.is_object() ||
          channel.schema.value("type", std::string{}) != "object")
        throw std::invalid_argument("invalid skill IPC channel declaration");
      nlohmann::json_schema::json_validator validator(
          nullptr, nlohmann::json_schema::default_string_format_check);
      validator.set_root_schema(channel.schema);
      if (!ipc_channels.insert(channel.name).second)
        throw std::invalid_argument("duplicate skill IPC channel declaration");
    }
    if ((!skill.accepted_signals.empty() || !skill.emitted_signals.empty()) &&
        !skill.supports_signals)
      throw std::invalid_argument("SIGNAL declarations require provider capability");
    std::set<std::tuple<ClaimKind, ClaimMode, std::string>> claims;
    for (const auto& claim : skill.claim_templates) {
      if (claim.argument_pointer.empty() || claim.argument_pointer.front() != '/')
        throw std::invalid_argument("claim argument pointer must be an RFC 6901 pointer");
      try {
        (void)nlohmann::json::json_pointer(claim.argument_pointer);
      } catch (...) {
        throw std::invalid_argument("invalid claim argument pointer");
      }
      if (!claims.emplace(claim.kind, claim.mode, claim.argument_pointer).second)
        throw std::invalid_argument("duplicate claim template");
    }
  }
}
const Metadata* Catalog::Find(const std::string& name, const std::string& version) const {
  for (const auto& skill : skills_)
    if (skill.name == name && skill.version == version) return &skill;
  return nullptr;
}
void Catalog::ValidateSignal(const SignalDescriptor& descriptor,
                             const nlohmann::json& payload) const {
  if (!payload.is_object()) throw std::invalid_argument("SIGNAL payload must be an object");
  nlohmann::json_schema::json_validator validator(
      nullptr, nlohmann::json_schema::default_string_format_check);
  validator.set_root_schema(descriptor.schema);
  validator.validate(payload);
}
std::vector<ResolvedClaim> ResolveClaims(const Metadata& skill, const nlohmann::json& arguments,
                                         const std::string& provider_context) {
  std::vector<ResolvedClaim> result;
  for (const auto& claim : skill.claim_templates) {
    const auto& value = arguments.at(nlohmann::json::json_pointer(claim.argument_pointer));
    if (!value.is_string() || value.get_ref<const std::string&>().empty())
      throw std::invalid_argument("claim value must resolve to a non-empty string");
    result.push_back({claim.kind, claim.mode, value.get<std::string>()});
  }
  if (!skill.reentrant) {
    if (provider_context.empty()) throw std::invalid_argument("provider context is required");
    result.push_back({ClaimKind::kProviderContext, ClaimMode::kExclusiveUse, provider_context});
  }
  return result;
}
}  // namespace eos::runtime::skills

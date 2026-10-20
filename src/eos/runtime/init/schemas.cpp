/// SchemaStore: startup loading and indexing of the canonical Agent-runtime
/// schemas, plus shared validation for dispatch and perception-model clients.

#include "src/eos/runtime/init/schemas.h"

#include <openssl/sha.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <iomanip>
#include <nlohmann/json-schema.hpp>
#include <regex>
#include <sstream>
#include <tuple>
#include <utility>
#include <vector>

#include "src/eos/lib/log.h"

namespace eos::runtime::init {
namespace {

std::uint16_t ReadLe16(const std::vector<unsigned char>& bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset]) | static_cast<std::uint16_t>(bytes[offset + 1])
                                                         << 8;
}

std::uint32_t ReadLe32(const std::vector<unsigned char>& bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset]) |
         static_cast<std::uint32_t>(bytes[offset + 1]) << 8 |
         static_cast<std::uint32_t>(bytes[offset + 2]) << 16 |
         static_cast<std::uint32_t>(bytes[offset + 3]) << 24;
}

std::vector<unsigned char> DecodeBase64Content(const std::string& value) {
  const auto Digit = [](unsigned char c) -> unsigned char {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    return c == '+' ? 62 : 63;
  };
  std::vector<unsigned char> decoded;
  decoded.reserve(value.size() / 4 * 3);
  std::uint32_t accumulator = 0;
  int bits = 0;
  for (const unsigned char c : value) {
    if (c == '=') break;
    accumulator = (accumulator << 6) | Digit(c);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      decoded.push_back(static_cast<unsigned char>((accumulator >> bits) & 0xff));
    }
  }
  return decoded;
}

void ValidatePcmWav(const std::vector<unsigned char>& bytes) {
  if (bytes.size() < 12 || std::string(bytes.begin(), bytes.begin() + 4) != "RIFF" ||
      std::string(bytes.begin() + 8, bytes.begin() + 12) != "WAVE") {
    throw std::runtime_error("audio/wav content is not RIFF/WAVE");
  }
  const std::size_t riff_end = static_cast<std::size_t>(ReadLe32(bytes, 4)) + 8;
  if (riff_end > bytes.size() || riff_end < 12) {
    throw std::runtime_error("audio/wav content has a truncated RIFF chunk");
  }

  bool found_format = false;
  bool found_audio = false;
  for (std::size_t offset = 12; offset + 8 <= riff_end;) {
    const std::string id(bytes.begin() + offset, bytes.begin() + offset + 4);
    const std::size_t size = ReadLe32(bytes, offset + 4);
    const std::size_t data = offset + 8;
    if (size > riff_end - data) {
      throw std::runtime_error("audio/wav content has a truncated chunk");
    }
    if (id == "fmt ") {
      if (size < 16 || ReadLe16(bytes, data) != 1 || ReadLe16(bytes, data + 2) != 1 ||
          ReadLe32(bytes, data + 4) != 16000 || ReadLe32(bytes, data + 8) != 32000 ||
          ReadLe16(bytes, data + 12) != 2 || ReadLe16(bytes, data + 14) != 16) {
        throw std::runtime_error("audio/wav content must be 16 kHz mono 16-bit linear PCM");
      }
      found_format = true;
    } else if (id == "data") {
      if (!found_format) {
        throw std::runtime_error("audio/wav content has a data chunk before its fmt chunk");
      }
      if (size == 0 || size % 2 != 0) {
        throw std::runtime_error("audio/wav content has no complete PCM samples");
      }
      found_audio = true;
    }
    const std::size_t padding = size % 2;
    if (data + size + padding > riff_end) {
      throw std::runtime_error("audio/wav content has a truncated chunk padding byte");
    }
    offset = data + size + padding;
  }
  if (!found_format || !found_audio) {
    throw std::runtime_error("audio/wav content requires fmt and data chunks");
  }
}

/// Enforces RFC 4648 syntax for every base64-encoded contract field. Media
/// types remain annotations here; capability-specific profiles run after the
/// selected schema has accepted the surrounding request.
void ContractContentCheck(const std::string& encoding, const std::string& /*media_type*/,
                          const nlohmann::json& instance) {
  if (encoding != "base64" || !instance.is_string()) return;
  const std::string value = instance.get<std::string>();
  const size_t padding = static_cast<size_t>(std::count(value.begin(), value.end(), '='));
  const size_t first_padding = value.find('=');
  for (const char c : value) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '+' || c == '/' || c == '=';
    if (!ok) {
      throw std::runtime_error("not a base64 string: '" + value + "'");
    }
  }
  if (value.size() % 4 != 0 || padding > 2 ||
      (first_padding != std::string::npos && first_padding != value.size() - padding)) {
    throw std::runtime_error("not a base64 string: '" + value + "'");
  }
}

/// The failing schema keyword for a validator message, or "" when unrecognized
/// (the schema path then names the subschema only). Coupled to pboettch
/// v2.3.0 message texts; an unknown message degrades the keyword, not the
/// instance path or the message itself.
std::string KeywordFor(const std::string& message) {
  static const std::pair<const char*, const char*> kKeywords[] = {
      {"required property", "required"},
      {"not found in required enum", "enum"},
      {"instance not const", "const"},
      {"unexpected instance type", "type"},
      {"too short as per minLength", "minLength"},
      {"too long as per maxLength", "maxLength"},
      {"does not match regex pattern", "pattern"},
      {"is not a multiple of", "multipleOf"},
      {"exceeds maximum", "maximum"},
      {"is below minimum", "minimum"},
      {"array has too many items", "maxItems"},
      {"array has too few items", "minItems"},
      {"too many properties", "maxProperties"},
      {"too few properties", "minProperties"},
      {"validation failed for additional property", "additionalProperties"},
      {"format-checking failed", "format"},
      {"content-checking failed", "contentEncoding"},
  };
  for (const auto& [needle, keyword] : kKeywords) {
    if (message.find(needle) != std::string::npos) {
      return keyword;
    }
  }
  return "";
}

/// The reference tokens of a JSON pointer, ~0/~1 escapes unescaped. The empty
/// pointer (the document root) yields no tokens.
std::vector<std::string> PointerTokens(const nlohmann::json::json_pointer& ptr) {
  std::vector<std::string> tokens;
  std::string text = ptr.to_string();
  if (text.empty()) {
    return tokens;
  }
  if (text[0] == '/') {
    text.erase(0, 1);
  }
  size_t start = 0;
  while (start <= text.size()) {
    size_t slash = text.find('/', start);
    if (slash == std::string::npos) {
      slash = text.size();
    }
    std::string token = text.substr(start, slash - start);
    size_t escape = 0;
    while ((escape = token.find("~", escape)) != std::string::npos) {
      if (escape + 1 < token.size() && token[escape + 1] == '1') {
        token.replace(escape, 2, "/");
      } else if (escape + 1 < token.size() && token[escape + 1] == '0') {
        token.replace(escape, 2, "~");
      }
      escape += 1;
    }
    tokens.push_back(std::move(token));
    start = slash + 1;
  }
  return tokens;
}

/// Resolves `#/$defs/...` style fragment references within the same document,
/// following ref chains up to a small depth cap. Returns nullptr when the
/// target does not resolve; `node` itself when it is not a reference.
const nlohmann::json* FollowRef(const nlohmann::json& root, const nlohmann::json& node) {
  const nlohmann::json* current = &node;
  for (int hops = 0; hops < 16 && current->is_object() && current->contains("$ref") &&
                     current->at("$ref").is_string();
       ++hops) {
    std::string ref = current->at("$ref").get<std::string>();
    if (!ref.empty() && ref[0] == '#') {
      ref.erase(0, 1);  // The fragment is a JSON pointer: "#/$defs/X" -> "/$defs/X".
    }
    if (ref.empty() || ref[0] != '/') {
      return nullptr;  // Not a same-document pointer (external refs don't occur here).
    }
    try {
      const nlohmann::json::json_pointer target(ref);
      if (!root.contains(target)) {
        return nullptr;
      }
      current = &root.at(target);
    } catch (const std::exception&) {
      return nullptr;
    }
  }
  return current;
}

/// The schema keyword path for an error at instance pointer `ptr`: walks the
/// raw schema document along the instance path (through `properties`, `items`,
/// and `$ref` indirections) and appends the failing keyword. Example: an enum
/// miss at "/state/posture" yields "#/properties/state/properties/posture/enum".
std::string SchemaPathFor(const nlohmann::json& root, const nlohmann::json::json_pointer& ptr,
                          const std::string& message) {
  std::string path = "#";
  const nlohmann::json* node = FollowRef(root, root);
  for (const std::string& token : PointerTokens(ptr)) {
    if (node == nullptr) {
      return path;
    }
    const bool has_property = node->contains("properties") && node->at("properties").is_object() &&
                              node->at("properties").contains(token);
    if (has_property) {
      node = FollowRef(root, node->at("properties").at(token));
      path += "/properties/" + token;
    } else if (node->contains("items") && node->at("items").is_object()) {
      node = FollowRef(root, node->at("items"));
      path += "/items";
    } else {
      return path;
    }
  }
  if (node == nullptr) {
    return path;
  }
  const std::string keyword = KeywordFor(message);
  if (!keyword.empty()) {
    path += "/" + keyword;
  }
  return path;
}

/// Collects every violation the validator reports instead of throwing on the
/// first one, stamping each with its schema keyword path.
class CollectingHandler final : public nlohmann::json_schema::error_handler {
 public:
  CollectingHandler(const nlohmann::json& schema, std::vector<SchemaIssue>& issues)
      : schema_(schema), issues_(issues) {}

  void error(const nlohmann::json::json_pointer& ptr, const nlohmann::json& /*instance*/,
             const std::string& message) override {
    issues_.push_back({SchemaPathFor(schema_, ptr, message), ptr.to_string(), message});
  }

 private:
  const nlohmann::json& schema_;
  std::vector<SchemaIssue>& issues_;
};

/// Reads one file to a string; false on I/O failure.
bool ReadFile(const std::filesystem::path& path, std::string* out) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  *out = buffer.str();
  return true;
}

std::string Sha256(const std::string& value) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest);
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (const unsigned char byte : digest) out << std::setw(2) << static_cast<unsigned>(byte);
  return out.str();
}

std::string ExpectedRole(const std::string& path) {
  if (path.rfind("actions/", 0) == 0) {
    return path == "actions/ack.v1.json" ? "acknowledgement" : "primitive";
  }
  if (path.rfind("body/", 0) == 0) return "body-device";
  if (path.rfind("world/", 0) == 0) return "world-resource";
  return {};
}

/// Every `*.vN.json` under `base`, as contracts-root-relative POSIX paths,
/// sorted so load order (and so collision and error reporting) is
/// deterministic.
std::vector<std::string> VersionedSchemas(const std::filesystem::path& root,
                                          const std::filesystem::path& base) {
  std::vector<std::string> found;
  std::error_code ec;
  std::filesystem::recursive_directory_iterator it(
      base, std::filesystem::directory_options::skip_permission_denied, ec);
  for (; !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    if (ec || !it->is_regular_file(ec)) {
      continue;
    }
    const std::string name = it->path().filename().string();
    static const std::regex kVersionedSchema(R"(^.+\.v[1-9][0-9]*\.json$)");
    if (!std::regex_match(name, kVersionedSchema)) {
      continue;
    }
    std::string rel = std::filesystem::relative(it->path(), root, ec).generic_string();
    if (ec) {
      break;
    }
    found.push_back(std::move(rel));
  }
  std::sort(found.begin(), found.end());
  return found;
}

/// Joins names for error messages: "a, b, c".
std::string JoinNames(const std::vector<std::string>& names) {
  std::string joined;
  for (const std::string& name : names) {
    if (!joined.empty()) {
      joined += ", ";
    }
    joined += name;
  }
  return joined;
}

/// Keys of a name-keyed map; std::map iterates in key order already.
template <typename Map>
std::vector<std::string> SortedKeys(const Map& map) {
  std::vector<std::string> keys;
  for (const auto& entry : map) {
    keys.push_back(entry.first);
  }
  return keys;
}

}  // namespace

std::string SchemaIssue::ToString() const {
  return instance_path.empty() ? message + " (schema " + schema_path + ")"
                               : instance_path + ": " + message + " (schema " + schema_path + ")";
}

bool PrimitiveIdentity::operator<(const PrimitiveIdentity& other) const {
  return std::tie(name, version) < std::tie(other.name, other.version);
}

bool PrimitiveIdentity::operator==(const PrimitiveIdentity& other) const {
  return name == other.name && version == other.version;
}

ValidationError::ValidationError(std::vector<SchemaIssue> issues)
    : std::runtime_error([&issues] {
        std::string what;
        for (const SchemaIssue& issue : issues) {
          if (!what.empty()) {
            what += "\n";
          }
          what += issue.ToString();
        }
        return what;
      }()),
      issues_(std::move(issues)) {}

struct SchemaStore::Entry {
  std::string rel_path;  ///< Generated-root-relative POSIX path, e.g. "actions/ack.v1.json".
  std::unique_ptr<nlohmann::json_schema::json_validator> validator;  ///< Compiled schema.
  nlohmann::json raw;  ///< Raw schema JSON, kept for error-path derivation.
  std::string role;
};

SchemaStore::SchemaStore(const std::filesystem::path& root, bool require_manifest) {
  // Callers may pass either the installed share directory or ext/eos directly.
  const std::filesystem::path schema_root =
      std::filesystem::is_directory(root / "ext/eos") ? root / "ext/eos" : root;
  static auto logger = eos::lib::GetLogger("contracts");
  eos::lib::LogEvent(logger, spdlog::level::info, "contract_load_started",
                     {{"root", root.string()}});
  struct ManifestEntry {
    std::string id;
    std::string role;
    std::string sha256;
    std::string name;
    std::string version;
  };
  std::map<std::string, ManifestEntry> manifest_entries;
  const std::filesystem::path manifest_path = schema_root / "contracts.manifest.json";
  std::string manifest_text;
  if (!ReadFile(manifest_path, &manifest_text)) {
    if (require_manifest) {
      throw ContractLoadError("contracts root has no generated manifest: " +
                              manifest_path.string());
    }
  } else {
    try {
      const nlohmann::json manifest = nlohmann::json::parse(manifest_text);
      if (manifest.value("format_version", 0) != 3 || !manifest.contains("schemas") ||
          !manifest.at("schemas").is_array()) {
        throw ContractLoadError("unsupported contract manifest: " + manifest_path.string());
      }
      for (const auto& item : manifest.at("schemas")) {
        const std::string path = item.at("path").get<std::string>();
        const std::string id = item.at("id").get<std::string>();
        const std::string role = item.at("role").get<std::string>();
        const std::string sha256 = item.at("sha256").get<std::string>();
        const std::string name = item.value("name", std::string());
        const std::string version = item.value("version", std::string());
        const std::string expected_role = ExpectedRole(path);
        if (expected_role.empty() || role != expected_role) {
          throw ContractLoadError("contract manifest role/path mismatch for " + path +
                                  ": expected '" + expected_role + "', found '" + role + "'");
        }
        if (sha256.size() != 64) {
          throw ContractLoadError("invalid sha256 in contract manifest for " + path);
        }
        if (role == "primitive" && (name.empty() || version.empty())) {
          throw ContractLoadError("primitive manifest entry has no name/version: " + path);
        }
        if (role != "primitive" && (!name.empty() || !version.empty())) {
          throw ContractLoadError("non-primitive manifest entry has primitive identity: " + path);
        }
        if (!manifest_entries.emplace(path, ManifestEntry{id, role, sha256, name, version})
                 .second) {
          throw ContractLoadError("duplicate path in contract manifest: " + path);
        }
      }
      if (manifest.value("schema_count", std::size_t{0}) != manifest_entries.size()) {
        throw ContractLoadError("contract manifest schema_count does not match its inventory: " +
                                manifest_path.string());
      }
      std::string digest_input;
      for (const auto& [path, entry] : manifest_entries) {
        digest_input += path + '\0' + entry.sha256 + '\n';
      }
      if (manifest.value("set_sha256", std::string()) != Sha256(digest_input)) {
        throw ContractLoadError("contract manifest set_sha256 is invalid: " +
                                manifest_path.string());
      }
    } catch (const ContractLoadError&) {
      throw;
    } catch (const std::exception& e) {
      throw ContractLoadError("invalid contract manifest: " + manifest_path.string() + ": " +
                              e.what());
    }
  }

  std::vector<std::string> discovered;
  for (const auto& [dir, base] :
       std::initializer_list<std::pair<const char*, std::filesystem::path>>{
           {"actions", schema_root / "actions"},
           {"body", schema_root / "body"},
           {"world", schema_root / "world"}}) {
    if (!std::filesystem::is_directory(base)) {
      throw ContractLoadError("contracts root has no '" + std::string(dir) +
                              "' tree: " + root.string());
    }
    const std::vector<std::string> directory_schemas = VersionedSchemas(schema_root, base);
    discovered.insert(discovered.end(), directory_schemas.begin(), directory_schemas.end());
    for (const std::string& rel : directory_schemas) {
      const std::string logical_rel = rel;
      const std::filesystem::path file = schema_root / rel;
      auto entry = std::make_unique<Entry>();
      entry->rel_path = logical_rel;

      std::string text;
      if (!ReadFile(file, &text)) {
        throw ContractLoadError("cannot read contract schema: " + file.string());
      }
      try {
        entry->raw = nlohmann::json::parse(text);
      } catch (const nlohmann::json::parse_error& e) {
        throw ContractLoadError("contract schema is not valid JSON: " + file.string() + ": " +
                                e.what());
      }
      if (!entry->raw.is_object() || !entry->raw.contains("$id") ||
          !entry->raw.at("$id").is_string()) {
        throw ContractLoadError("contract schema has no string $id: " + file.string());
      }

      const std::string id = entry->raw.at("$id").get<std::string>();
      if (!manifest_entries.empty()) {
        const auto expected = manifest_entries.find(rel);
        if (expected == manifest_entries.end()) {
          throw ContractLoadError("contract schema is absent from manifest: " + rel);
        }
        if (expected->second.id != id) {
          throw ContractLoadError("contract schema $id differs from manifest for " + rel +
                                  ": expected '" + expected->second.id + "', found '" + id + "'");
        }
        if (expected->second.sha256 != Sha256(text)) {
          throw ContractLoadError("contract schema content hash differs from manifest for " + rel);
        }
        entry->role = expected->second.role;
      } else {
        entry->role = ExpectedRole(rel);
      }
      if (by_key_.count(id) != 0) {
        throw ContractLoadError("duplicate contract $id '" + id + "': " + by_key_.at(id)->rel_path +
                                " and " + rel);
      }
      if (by_key_.count(logical_rel) != 0) {
        throw ContractLoadError("duplicate contract path '" + logical_rel + "'");
      }

      // The type const, where present, is the wire discriminator every map
      // below is keyed by — never the filename.
      const std::string type_const =
          entry->raw.value(nlohmann::json::json_pointer("/properties/type/const"), std::string());
      Entry* loaded = entry.get();
      by_key_[id] = loaded;
      by_key_[logical_rel] = loaded;
      entries_.push_back(std::move(entry));
      eos::lib::LogEvent(logger, spdlog::level::debug, "contract_schema_indexed",
                         {{"path", rel}, {"id", id}});

      if (loaded->role == "body-device") {
        if (!type_const.empty()) {
          throw ContractLoadError("body-device schema must not declare a resource type: " + rel);
        }
        continue;
      }
      if (type_const.empty()) {
        throw ContractLoadError("manifest role '" + loaded->role +
                                "' requires a type discriminator: " + rel);
      }
      if (loaded->role == "primitive" || loaded->role == "acknowledgement") {
        if (loaded->role == "acknowledgement") {
          if (type_const != "primitive.ack") {
            throw ContractLoadError("acknowledgement role must use primitive.ack: " + rel);
          }
          if (ack_ != nullptr) {
            throw ContractLoadError("duplicate primitive.ack schema: " + ack_->rel_path + " and " +
                                    rel);
          }
          ack_ = loaded;
        } else {
          const std::string primitive_version =
              loaded->raw.value("x-island-primitive-version", std::string());
          if (primitive_version.empty()) {
            throw ContractLoadError("primitive schema has no x-island-primitive-version: " + rel);
          }
          const PrimitiveIdentity identity{type_const, primitive_version};
          if (!manifest_entries.empty()) {
            const auto& expected = manifest_entries.at(rel);
            if (expected.name != identity.name || expected.version != identity.version) {
              throw ContractLoadError("primitive identity differs from manifest for " + rel);
            }
          }
          if (primitives_.count(identity) != 0) {
            throw ContractLoadError("duplicate primitive identity '" + identity.name + "@" +
                                    identity.version + "': " + primitives_.at(identity)->rel_path +
                                    " and " + rel);
          }
          primitives_[identity] = loaded;
        }
      } else if (loaded->role == "world-resource") {
        if (resources_.count(type_const) != 0) {
          throw ContractLoadError("duplicate resource type '" + type_const +
                                  "': " + resources_.at(type_const)->rel_path + " and " + rel);
        }
        resources_[type_const] = loaded;
      } else {
        throw ContractLoadError("unknown contract manifest role '" + loaded->role + "': " + rel);
      }
    }
  }
  if (!manifest_entries.empty()) {
    std::sort(discovered.begin(), discovered.end());
    std::vector<std::string> declared;
    for (const auto& [path, unused] : manifest_entries) declared.push_back(path);
    if (discovered != declared) {
      throw ContractLoadError("contract manifest inventory differs from installed schema tree: " +
                              manifest_path.string());
    }
  }
  // Compile only after every schema has been indexed. This lets resource
  // contracts reuse definitions from another canonical schema by `$id`
  // without making load order significant.
  const auto loader = [this](const nlohmann::json_uri& uri, nlohmann::json& schema) {
    std::vector<std::string> keys{uri.url(), uri.path()};
    if (!uri.url().empty() && uri.url().front() == '/') keys.push_back(uri.url().substr(1));
    if (!uri.path().empty() && uri.path().front() == '/') keys.push_back(uri.path().substr(1));
    for (const std::string& key : keys) {
      const auto found = by_key_.find(key);
      if (found != by_key_.end()) {
        schema = found->second->raw;
        return;
      }
    }
    throw std::invalid_argument("unknown canonical schema reference '" + uri.url() + "'");
  };
  for (const auto& entry : entries_) {
    entry->validator = std::make_unique<nlohmann::json_schema::json_validator>(
        loader, nlohmann::json_schema::default_string_format_check, ContractContentCheck);
    try {
      entry->validator->set_root_schema(entry->raw);
    } catch (const std::exception& e) {
      throw ContractLoadError("contract schema does not compile: " + entry->rel_path + ": " +
                              e.what());
    }
  }
  if (ack_ == nullptr) {
    throw ContractLoadError("contracts root has no primitive.ack schema under actions/");
  }
  eos::lib::LogEvent(logger, spdlog::level::info, "contract_schemas_loaded",
                     {{"count", entries_.size()},
                      {"root", root.string()},
                      {"primitives", primitives_.size()},
                      {"resource_types", resources_.size()}});
}

SchemaStore::~SchemaStore() = default;

std::vector<PrimitiveIdentity> SchemaStore::PrimitiveIdentities() const {
  std::vector<PrimitiveIdentity> identities;
  identities.reserve(primitives_.size());
  for (const auto& [identity, unused] : primitives_) identities.push_back(identity);
  return identities;
}

bool SchemaStore::HasPrimitive(const std::string& primitive, const std::string& version) const {
  return primitives_.find({primitive, version}) != primitives_.end();
}

nlohmann::json SchemaStore::PrimitiveSchema(const std::string& primitive,
                                             const std::string& version) const {
  const PrimitiveIdentity identity{primitive, version};
  const auto it = primitives_.find(identity);
  if (it == primitives_.end()) {
    std::vector<std::string> known;
    for (const auto& [entry, unused] : primitives_)
      known.push_back(entry.name + "@" + entry.version);
    ThrowUnknown("primitive", primitive + "@" + version, known);
  }
  return it->second->raw;
}

std::vector<std::string> SchemaStore::ResourceTypes() const { return SortedKeys(resources_); }

void SchemaStore::ValidateWith(const Entry& entry, const nlohmann::json& instance) const {
  std::vector<SchemaIssue> issues;
  CollectingHandler handler(entry.raw, issues);
  entry.validator->validate(instance, handler);
  if (!issues.empty()) {
    static auto logger = eos::lib::GetLogger("contracts");
    eos::lib::LogEvent(logger, spdlog::level::warn, "contract_validation_failed",
                       {{"contract", entry.rel_path}, {"issue_count", issues.size()}});
    throw ValidationError(std::move(issues));
  }
}

void SchemaStore::ThrowUnknown(const std::string& what, const std::string& given,
                               const std::vector<std::string>& known) {
  std::vector<SchemaIssue> issues;
  issues.push_back(
      {"#", "", "unknown " + what + " '" + given + "'; known " + what + "s: " + JoinNames(known)});
  throw ValidationError(std::move(issues));
}

void SchemaStore::ValidatePrimitive(const std::string& primitive, const std::string& version,
                                    const nlohmann::json& instance) const {
  const PrimitiveIdentity identity{primitive, version};
  const auto it = primitives_.find(identity);
  if (it == primitives_.end()) {
    std::vector<std::string> known;
    for (const auto& [entry, unused] : primitives_)
      known.push_back(entry.name + "@" + entry.version);
    ThrowUnknown("primitive", primitive + "@" + version, known);
  }
  ValidateWith(*it->second, instance);
}

void SchemaStore::ValidateResource(const std::string& type, const nlohmann::json& instance) const {
  const auto it = resources_.find(type);
  if (it == resources_.end()) {
    ThrowUnknown("resource type", type, SortedKeys(resources_));
  }
  ValidateWith(*it->second, instance);
}

void SchemaStore::ValidateAck(const nlohmann::json& instance) const {
  ValidateWith(*ack_, instance);
}

void SchemaStore::ValidateById(const std::string& id, const nlohmann::json& instance) const {
  const auto it = by_key_.find(id);
  if (it == by_key_.end()) {
    ThrowUnknown("schema id", id, SortedKeys(by_key_));
  }
  ValidateWith(*it->second, instance);
  if (it->second->rel_path == "perception/model/vad_request.v1.json") {
    try {
      ValidatePcmWav(DecodeBase64Content(instance.at("input").at("data_b64").get<std::string>()));
    } catch (const std::runtime_error& error) {
      throw ValidationError({{"#/properties/input/properties/data_b64/contentMediaType",
                              "/input/data_b64", error.what()}});
    }
  }
}

}  // namespace eos::runtime::init

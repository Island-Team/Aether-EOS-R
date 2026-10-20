/// Immutable, thread-safe contract schemas. Loading failures refuse startup.

#ifndef EOS_RUNTIME_INIT_SCHEMAS_H_
#define EOS_RUNTIME_INIT_SCHEMAS_H_

#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace eos::runtime::init {

/// Exact identity of one versioned primitive contract.
struct PrimitiveIdentity {
  std::string name;
  std::string version;

  bool operator<(const PrimitiveIdentity& other) const;
  bool operator==(const PrimitiveIdentity& other) const;
};

/// Paths and message for one validation violation.
struct SchemaIssue {
  std::string schema_path;    ///< Keyword path in the schema, e.g. "#/properties/state/required".
  std::string instance_path;  ///< JSON pointer to the value, e.g. "/state/posture"; "" is the root.
  std::string
      message;  ///< Validator message, e.g. "required property 'sequence' not found in object".

  /// One loggable line: "<instance_path>: <message> (schema <schema_path>)".
  std::string ToString() const;
};

/// what() joins violations; Issues() retains their structure.
class ValidationError : public std::runtime_error {
 public:
  /// Builds the error from the collected issues; every issue lands in what().
  explicit ValidationError(std::vector<SchemaIssue> issues);

  /// Every violation found in the rejected instance, in report order.
  const std::vector<SchemaIssue>& Issues() const { return issues_; }

 private:
  std::vector<SchemaIssue> issues_;
};

/// Load failure; what() names the offending file.
class ContractLoadError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// Indexes derive from schema content; `$ref` resolves by `$id` after indexing.
class SchemaStore {
 public:
  /// Verifies manifest inventory and hashes before compilation.
  /// `require_manifest=false` is for isolated schema tests only.
  explicit SchemaStore(const std::filesystem::path& root, bool require_manifest = true);
  ~SchemaStore();
  SchemaStore(const SchemaStore&) = delete;
  SchemaStore& operator=(const SchemaStore&) = delete;

  /// Sorted primitive identities the store can validate — the derived dispatch set.
  std::vector<PrimitiveIdentity> PrimitiveIdentities() const;

  /// True when an exact primitive identity has a loaded action schema.
  bool HasPrimitive(const std::string& primitive, const std::string& version) const;

  /// Returns a copy of the canonical schema for an exact primitive identity.
  /// Throws ValidationError when the identity is not loaded.
  nlohmann::json PrimitiveSchema(const std::string& primitive,
                                 const std::string& version) const;

  /// Sorted resource types the store can validate via their type const.
  std::vector<std::string> ResourceTypes() const;

  /// Number of schemas loaded across generated actions/, body/, and world/.
  std::size_t Size() const { return entries_.size(); }

  /// Validates a primitive request against the action schema selected by name.
  void ValidatePrimitive(const std::string& primitive, const std::string& version,
                         const nlohmann::json& instance) const;

  /// Validates a resource snapshot against the perception schema its type
  /// const selects (e.g. "base.snapshot"). Throws ValidationError; an unknown
  /// type is itself an error naming the known types.
  void ValidateResource(const std::string& type, const nlohmann::json& instance) const;

  /// Validates a primitive acknowledgement against actions/ack.v1.json.
  void ValidateAck(const nlohmann::json& instance) const;

  /// Validates an instance against the schema its `$id` or root-relative path
  /// selects. Throws ValidationError; an unknown id is itself an error naming
  /// the known ids.
  void ValidateById(const std::string& id, const nlohmann::json& instance) const;

 private:
  /// One compiled schema plus the raw JSON it was built from (kept to derive
  /// schema keyword paths for validation errors). Defined in schemas.cpp.
  struct Entry;

  /// Validator core shared by every Validate* call: runs the entry's compiled
  /// schema over the instance and throws a ValidationError carrying every
  /// issue found.
  void ValidateWith(const Entry& entry, const nlohmann::json& instance) const;

  /// Sorted-name error helper for unknown keys, e.g.
  /// "unknown primitive 'arm.wave'; known primitives: arm.movel, ...".
  [[noreturn]] static void ThrowUnknown(const std::string& what, const std::string& given,
                                        const std::vector<std::string>& known);

  std::vector<std::unique_ptr<Entry>> entries_;     ///< Loaded schemas, in load order.
  std::map<std::string, Entry*> by_key_;            ///< $id and relative path -> schema.
  std::map<PrimitiveIdentity, Entry*> primitives_;  ///< exact identity -> action schema (no ack).
  std::map<std::string, Entry*> resources_;         ///< type const -> perception schema.
  Entry* ack_ = nullptr;                            ///< The primitive.ack schema.
};

}  // namespace eos::runtime::init

#endif  // EOS_RUNTIME_INIT_SCHEMAS_H_

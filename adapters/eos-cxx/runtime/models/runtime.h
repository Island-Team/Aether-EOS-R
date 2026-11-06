/// Configured outbound clients for small perception-model services.
#ifndef EOS_RUNTIME_PERCEPTION_MODELS_RUNTIME_H_
#define EOS_RUNTIME_PERCEPTION_MODELS_RUNTIME_H_

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eos::runtime::models {

enum class Capability {
  kVad,
  kAsr,
  kTts,
  kDetection,
  kHuman,
  kHumanPose,
  // Retained until init/runtime.cpp migrates to the typed stream switches.
  kPose,
  kPosture,
  kPostureClassification,
  kVlm,
  kFace,
  kArmGraspPlan,
  kChatCompletion,
};

enum class WireFormat {
  kJson,
  kMultipart,
};

enum class Transport {
  kHttp,
  kWebSocket,
  kGrpc,
};

enum class HttpMethod {
  kPost,
  kGet,
  kDelete,
};

/// Configured model endpoint.
struct ModelConfig {
  std::string id;
  bool enabled{true};
  Capability capability{Capability::kVad};
  std::string endpoint;
  /// Optional health probe endpoint. Omission leaves health unavailable.
  std::string health_endpoint;
  std::chrono::milliseconds health_interval{30000};
  std::chrono::milliseconds health_timeout{2000};
  HttpMethod http_method{HttpMethod::kPost};
  std::chrono::milliseconds timeout{3000};
  Transport transport{Transport::kHttp};
  WireFormat wire_format{WireFormat::kJson};
  /// External adapter selected at composition time. The core does not
  /// interpret provider names or provider-specific request fields.
  std::string adapter{"canonical"};
  std::string api_key_env;
  std::string provider_model_id;
  std::string file_field{"file"};
  std::size_t max_request_bytes{16 * 1024 * 1024};
  std::size_t max_response_bytes{16 * 1024 * 1024};
  std::vector<std::pair<std::string, std::string>> fields;
  std::string request_contract;
  std::string response_contract;
};

struct CallResult;
using CancelRequested = std::function<bool()>;

/// Provider-owned WebSocket session.
class StreamSession {
 public:
  virtual ~StreamSession() = default;
  virtual CallResult Call(const nlohmann::json& request,
                          const CancelRequested& cancel_requested) = 0;
  virtual void Close() noexcept = 0;
};

enum class HealthStatus {
  kChecking,
  kReady,
  kDegraded,
  kUnavailable,
};

/// Cached provider health, without endpoints or credentials.
struct ModelHealth {
  std::string model_id;
  HealthStatus status{HealthStatus::kChecking};
  std::string interface_version;
  std::string service_name;
  std::string service_version;
  std::string provider_model_id;
  std::string model_version;
  std::vector<std::string> capabilities;
  std::string message;
  std::chrono::system_clock::time_point checked_at{};
};

/// Distinguishes disabled models from missing or mismatched IDs.
enum class ModelReferenceState {
  kEnabled,
  kDisabled,
  kMissing,
  kWrongCapability,
};

/// Test override; production loads private model schemas.
using ContractValidator = std::function<void(const std::string&, const nlohmann::json&)>;

/// Provider adapter. Requests and normalized responses pass canonical schemas.
class Adapter {
 public:
  virtual ~Adapter() = default;
  virtual nlohmann::json PrepareRequest(const ModelConfig& model,
                                        const nlohmann::json& request) const = 0;
  virtual nlohmann::json DecodeResponse(const ModelConfig& model, std::string_view media_type,
                                        std::string_view body) const = 0;
  virtual void NormalizeResponse(const ModelConfig& model, nlohmann::json* response) const = 0;
  virtual void NormalizeResponseForRequest(const ModelConfig& model,
                                           const nlohmann::json& request,
                                           nlohmann::json* response) const {
    NormalizeResponse(model, response);
  }
  virtual void NormalizeHealthResponse(const ModelConfig&, nlohmann::json*) const {}
  virtual std::unique_ptr<StreamSession> CreateStreamSession(const ModelConfig&) const {
    return {};
  }
};

using AdapterFactory = std::function<std::shared_ptr<const Adapter>(const ModelConfig& model)>;

struct CallResult {
  enum class Status {
    kCompleted,
    kInvalidArgument,
    kNotFound,
    kUnavailable,
    kDeadlineExceeded,
    kRemoteError,
    kProtocolError,
    kInternal,
  };

  Status status{Status::kInternal};
  unsigned http_status{0};
  nlohmann::json response;
  std::string message;
};

using Completion = std::function<void(CallResult)>;

std::string_view CapabilityName(Capability capability);

/// Parses `[[perception.model]]` entries from eos.toml. `enabled` defaults to
/// true; disabled entries are retained and strictly validated. A missing
/// `[perception]` section yields an empty registry.
/// Malformed entries throw std::runtime_error naming `path` and the rejected key.
std::vector<ModelConfig> ParseConfig(std::string_view text, const std::string& path);

/// Thread-safe blocking model client; unsuitable for real-time loops.
/// WebSocket calls reuse a serialized connection per model.
class Runtime final {
 public:
  Runtime();
  Runtime(std::vector<ModelConfig> models, ContractValidator validate = {},
          AdapterFactory adapter_factory = {});
  ~Runtime();
  Runtime(Runtime&&) noexcept;
  Runtime& operator=(Runtime&&) noexcept;
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  /// Enabled models in lexical ID order.
  std::vector<ModelConfig> Discover() const;
  /// All configured models in lexical ID order, including disabled entries.
  std::vector<ModelConfig> ConfiguredAll() const;
  /// All configured entries, including disabled ones, without endpoint secrets.
  std::optional<ModelConfig> Configured(const std::string& id) const;

  /// Exact enabled model-ID lookup, or nullptr when absent or disabled. The
  /// pointer remains valid for this Runtime's lifetime.
  const ModelConfig* Find(const std::string& id) const;

  /// Resolves one exact configured ID for an internal runtime dependency.
  /// This never selects another model with the same capability.
  ModelReferenceState Resolve(const std::string& id, Capability expected) const;

  /// Returns the latest health probe result for an enabled model.
  std::optional<ModelHealth> Health(const std::string& id) const;

  /// Returns cached health for all enabled models in lexical model-ID order.
  std::vector<ModelHealth> HealthAll() const;

  /// Validates `request`, invokes the configured endpoint, parses the response,
  /// then validates its schema and capability-level invariants.
  /// Network and protocol failures are returned as data; this method does not
  /// throw for request-time failures.
  CallResult Call(const std::string& id, const nlohmann::json& request) const;
  CallResult Call(const std::string& id, const nlohmann::json& request,
                  const CancelRequested& cancel_requested) const;

  /// Performs the internal ArcFace registration protocol. Registration is an
  /// EOS primitive dependency, not part of the `vision.face` inference contract.
  CallResult RegisterFace(const std::string& id, const nlohmann::json& input,
                          const std::string& name) const;

  /// Queues a call on the module-owned bounded worker pool. Returns false
  /// when the runtime is stopping or its queue is full. An optional cooperative
  /// cancellation probe aborts queued or active network work.
  bool CallAsync(std::string id, nlohmann::json request, Completion complete,
                 CancelRequested cancel_requested = {});

 private:
  CallResult CallCancellable(const std::string& id, const nlohmann::json& request,
                             const CancelRequested& cancel_requested) const;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace eos::runtime::models

#endif  // EOS_RUNTIME_PERCEPTION_MODELS_RUNTIME_H_

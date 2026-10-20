/// Header-only C++17 authoring facade for the stable EOS native skill ABI.
#ifndef EOS_SKILL_SDK_HPP_
#define EOS_SKILL_SDK_HPP_

#include <eos/skill/abi.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace eos::skill {

using Json = nlohmann::json;

enum class ErrorCode {
  kInvalidArgument,
  kNotFound,
  kUnavailable,
  kInternal,
  kMalformedResponse,
  kClosed,
};

struct Error {
  ErrorCode code{ErrorCode::kInternal};
  std::string message;
};

template <typename T>
class Result {
 public:
  static Result Success(T value) { return Result(std::move(value)); }
  static Result Failure(ErrorCode code, std::string message) {
    return Result(Error{code, std::move(message)});
  }

  explicit operator bool() const { return value_.has_value(); }
  bool ok() const { return value_.has_value(); }
  T& value() { return value_.value(); }
  const T& value() const { return value_.value(); }
  T&& take() { return std::move(value_.value()); }
  const Error& error() const { return error_.value(); }

 private:
  explicit Result(T value) : value_(std::move(value)) {}
  explicit Result(Error error) : error_(std::move(error)) {}
  std::optional<T> value_;
  std::optional<Error> error_;
};

template <>
class Result<void> {
 public:
  static Result Success() { return Result(); }
  static Result Failure(ErrorCode code, std::string message) {
    return Result(Error{code, std::move(message)});
  }
  explicit operator bool() const { return !error_.has_value(); }
  bool ok() const { return !error_.has_value(); }
  const Error& error() const { return error_.value(); }

 private:
  Result() = default;
  explicit Result(Error error) : error_(std::move(error)) {}
  std::optional<Error> error_;
};

struct PrimitiveRef {
  std::string name;
  std::string version;
};

struct PrimitiveOperation {
  std::uint64_t id{0};
};

struct Signal {
  std::string name;
  Json payload = Json::object();
};
enum class SourceLookup { kLatest, kExact, kBefore, kAfter, kNearest };
struct SourceSample {
  std::string topic;
  std::int64_t source_timestamp_ns{0};
  std::uint64_t sequence{0};
  Json value = Json::object();
};
struct RetainedFrame {
  std::string source;
  std::uint64_t source_sequence{0};
  std::uint64_t source_generation{0};
};
struct VadInference { static constexpr eos_skill_inference_authority_v2 authority = EOS_SKILL_INFERENCE_VAD; };
struct HumanInference { static constexpr eos_skill_inference_authority_v2 authority = EOS_SKILL_INFERENCE_HUMAN; };
struct HumanPoseInference { static constexpr eos_skill_inference_authority_v2 authority = EOS_SKILL_INFERENCE_HUMAN_POSE; };
struct FaceRecognitionInference { static constexpr eos_skill_inference_authority_v2 authority = EOS_SKILL_INFERENCE_FACE_RECOGNITION; };
struct ChatCompletionInference { static constexpr eos_skill_inference_authority_v2 authority = EOS_SKILL_INFERENCE_CHAT_COMPLETION; };
struct VlmInference { static constexpr eos_skill_inference_authority_v2 authority = EOS_SKILL_INFERENCE_VLM; };

class CheckpointOptions {
 public:
  CheckpointOptions& Message(std::string value) {
    message_ = std::move(value);
    return *this;
  }
  CheckpointOptions& Progress(double value) {
    progress_ = value;
    return *this;
  }
  CheckpointOptions& Fields(Json value) {
    fields_ = std::move(value);
    return *this;
  }
  const std::string& message() const { return message_; }
  const std::optional<double>& progress() const { return progress_; }
  const Json& fields() const { return fields_; }

 private:
  std::string message_;
  std::optional<double> progress_;
  Json fields_ = Json::object();
};

class Outcome {
 public:
  enum class Kind { kSuccess, kRejected, kCancelled, kInvalidArgument, kFailure };

  static Outcome Success(std::string reason, Json result = Json::object(),
                         std::string message = {}) {
    return Outcome(Kind::kSuccess, std::move(reason), std::move(result), std::move(message));
  }
  static Outcome Reject(std::string reason, Json result = Json::object(),
                        std::string message = {}) {
    return Outcome(Kind::kRejected, std::move(reason), std::move(result), std::move(message));
  }
  static Outcome Cancelled() { return Outcome(Kind::kCancelled, {}, Json::object(), {}); }
  static Outcome InvalidArgument(std::string reason = "arguments.invalid",
                                 std::string message = {}) {
    return Outcome(Kind::kInvalidArgument, std::move(reason), Json::object(), std::move(message));
  }
  static Outcome Failure(std::string reason = "skill.failed", std::string message = {}) {
    return Outcome(Kind::kFailure, std::move(reason), Json::object(), std::move(message));
  }

  Kind kind() const { return kind_; }
  const std::string& reason() const { return reason_; }
  const Json& result() const { return result_; }
  const std::string& message() const { return message_; }

 private:
  Outcome(Kind kind, std::string reason, Json result, std::string message)
      : kind_(kind),
        reason_(std::move(reason)),
        result_(std::move(result)),
        message_(std::move(message)) {}
  Kind kind_;
  std::string reason_;
  Json result_;
  std::string message_;
};

namespace detail {

inline constexpr std::size_t kMaximumTextBytes =
    static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
inline constexpr std::size_t kMaximumEventBytes = 128;
inline constexpr std::size_t kMaximumMessageBytes = 1024;
inline constexpr std::size_t kMaximumObservabilityJsonBytes = 16 * 1024;

inline eos_skill_text_v2 Borrow(std::string_view value) {
  if (value.empty()) return {nullptr, 0};
  return {value.data(), static_cast<std::uint32_t>(value.size())};
}

inline Result<std::string> Copy(eos_skill_text_v2 value, const char* field) {
  if (value.size == 0) return Result<std::string>::Success({});
  if (value.data == nullptr)
    return Result<std::string>::Failure(ErrorCode::kMalformedResponse,
                                        std::string(field) + " has null data");
  return Result<std::string>::Success(std::string(value.data, value.size));
}

inline ErrorCode HostError(eos_skill_host_status_v2 status) {
  switch (status) {
    case EOS_SKILL_HOST_INVALID_ARGUMENT:
      return ErrorCode::kInvalidArgument;
    case EOS_SKILL_HOST_NOT_FOUND:
      return ErrorCode::kNotFound;
    case EOS_SKILL_HOST_UNAVAILABLE:
      return ErrorCode::kUnavailable;
    default:
      return ErrorCode::kInternal;
  }
}

inline bool Fits(std::string_view value) { return value.size() <= kMaximumTextBytes; }

inline Result<std::string> EncodeObject(const Json& value, const char* field) {
  if (!value.is_object())
    return Result<std::string>::Failure(ErrorCode::kInvalidArgument,
                                        std::string(field) + " must be a JSON object");
  try {
    std::string encoded = value.dump();
    if (!Fits(encoded))
      return Result<std::string>::Failure(ErrorCode::kInvalidArgument,
                                          std::string(field) + " is too large");
    return Result<std::string>::Success(std::move(encoded));
  } catch (const std::exception& error) {
    return Result<std::string>::Failure(ErrorCode::kInvalidArgument, error.what());
  }
}

inline Result<Json> ParseObject(eos_skill_text_v2 value, const char* field) {
  auto copied = Copy(value, field);
  if (!copied) return Result<Json>::Failure(copied.error().code, copied.error().message);
  try {
    Json parsed = Json::parse(copied.value());
    if (!parsed.is_object())
      return Result<Json>::Failure(ErrorCode::kMalformedResponse,
                                   std::string(field) + " must be a JSON object");
    return Result<Json>::Success(std::move(parsed));
  } catch (const std::exception& error) {
    return Result<Json>::Failure(ErrorCode::kMalformedResponse, error.what());
  }
}

}  // namespace detail

class Execution;

class Logger {
 public:
  Result<void> trace(std::string event, std::string message = {},
                     Json fields = Json::object()) const;
  Result<void> debug(std::string event, std::string message = {},
                     Json fields = Json::object()) const;
  Result<void> info(std::string event, std::string message = {},
                    Json fields = Json::object()) const;
  Result<void> warn(std::string event, std::string message = {},
                    Json fields = Json::object()) const;
  Result<void> error(std::string event, std::string message = {},
                     Json fields = Json::object()) const;
  Result<void> critical(std::string event, std::string message = {},
                        Json fields = Json::object()) const;

 private:
  friend class Execution;
  explicit Logger(Execution* execution) : execution_(execution) {}
  Result<void> Write(eos_skill_log_level_v2 level, std::string event, std::string message,
                     Json fields) const;
  Execution* execution_;
};

class Execution {
 public:
  explicit Execution(const eos_skill_execution_v2& execution) : raw_(&execution), logger_(this) {
    if (execution.struct_size < sizeof(eos_skill_execution_v2) || execution.host == nullptr ||
        execution.host->abi_version != EOS_SKILL_ABI_VERSION_V2 ||
        execution.host->struct_size < EOS_SKILL_HOST_V2_BASE_SIZE)
      throw std::invalid_argument("incompatible EOS skill execution");
    auto command = detail::Copy(execution.command_id, "command_id");
    if (!command) throw std::invalid_argument(command.error().message);
    command_id_ = command.take();
    auto arguments = detail::ParseObject(execution.arguments_json, "arguments_json");
    if (!arguments) throw std::invalid_argument(arguments.error().message);
    arguments_ = arguments.take();
  }

  Execution(const Execution&) = delete;
  Execution& operator=(const Execution&) = delete;
  const Json& arguments() const { return arguments_; }
  const std::string& command_id() const { return command_id_; }
  Logger& logger() { return logger_; }
  const Logger& logger() const { return logger_; }
  bool cancellation_available() const { return raw_->host->is_cancelled != nullptr; }

  bool cancelled() const {
    return raw_->host->is_cancelled != nullptr &&
           raw_->host->is_cancelled(raw_->host->context) != 0;
  }

  Result<Json> call_primitive(const PrimitiveRef& primitive, std::string_view device_id,
                              const Json& arguments) const {
    if (raw_->host->call_primitive == nullptr)
      return Result<Json>::Failure(ErrorCode::kUnavailable, "primitive callback unavailable");
    if (!detail::Fits(primitive.name) || !detail::Fits(primitive.version) ||
        !detail::Fits(device_id))
      return Result<Json>::Failure(ErrorCode::kInvalidArgument, "primitive text is too large");
    auto encoded = detail::EncodeObject(arguments, "primitive arguments");
    if (!encoded) return Result<Json>::Failure(encoded.error().code, encoded.error().message);
    eos_skill_primitive_request_v2 request{
        sizeof(request), detail::Borrow(primitive.name), detail::Borrow(primitive.version),
        detail::Borrow(device_id), detail::Borrow(encoded.value())};
    eos_skill_primitive_response_v2 response{sizeof(response), {}, {}};
    const auto status = raw_->host->call_primitive(raw_->host->context, &request, &response);
    if (status != EOS_SKILL_HOST_OK) {
      auto message = detail::Copy(response.error_message, "primitive error");
      return Result<Json>::Failure(detail::HostError(status),
                                   message ? message.take() : "primitive call failed");
    }
    return detail::ParseObject(response.acknowledgement_json, "primitive acknowledgement");
  }

  Result<PrimitiveOperation> start_primitive(const PrimitiveRef& primitive,
                                             std::string_view device_id,
                                             const Json& arguments) const {
    if (!EOS_SKILL_HOST_V2_HAS(raw_->host, start_primitive) ||
        raw_->host->start_primitive == nullptr ||
        !EOS_SKILL_HOST_V2_HAS(raw_->host, wait_primitive) ||
        raw_->host->wait_primitive == nullptr ||
        !EOS_SKILL_HOST_V2_HAS(raw_->host, cancel_primitive) ||
        raw_->host->cancel_primitive == nullptr) {
      return Result<PrimitiveOperation>::Failure(ErrorCode::kUnavailable,
                                                 "asynchronous primitive callbacks unavailable");
    }
    if (!detail::Fits(primitive.name) || !detail::Fits(primitive.version) ||
        !detail::Fits(device_id)) {
      return Result<PrimitiveOperation>::Failure(ErrorCode::kInvalidArgument,
                                                 "primitive text is too large");
    }
    auto encoded = detail::EncodeObject(arguments, "primitive arguments");
    if (!encoded) {
      return Result<PrimitiveOperation>::Failure(encoded.error().code, encoded.error().message);
    }
    eos_skill_primitive_request_v2 request{
        sizeof(request), detail::Borrow(primitive.name), detail::Borrow(primitive.version),
        detail::Borrow(device_id), detail::Borrow(encoded.value())};
    eos_skill_primitive_start_response_v2 response{sizeof(response), 0, {}};
    const auto status = raw_->host->start_primitive(raw_->host->context, &request, &response);
    if (status != EOS_SKILL_HOST_OK) {
      auto message = detail::Copy(response.error_message, "primitive start error");
      return Result<PrimitiveOperation>::Failure(
          detail::HostError(status), message ? message.take() : "primitive start failed");
    }
    if (response.operation_id == 0) {
      return Result<PrimitiveOperation>::Failure(ErrorCode::kMalformedResponse,
                                                 "primitive operation ID is zero");
    }
    return Result<PrimitiveOperation>::Success({response.operation_id});
  }

  Result<std::optional<Json>> wait_primitive(
      PrimitiveOperation operation, std::chrono::milliseconds timeout) const {
    if (!EOS_SKILL_HOST_V2_HAS(raw_->host, wait_primitive) ||
        raw_->host->wait_primitive == nullptr) {
      return Result<std::optional<Json>>::Failure(ErrorCode::kUnavailable,
                                                  "primitive wait callback unavailable");
    }
    if (operation.id == 0 || timeout.count() < 0 ||
        static_cast<std::uint64_t>(timeout.count()) >
            std::numeric_limits<std::uint32_t>::max()) {
      return Result<std::optional<Json>>::Failure(ErrorCode::kInvalidArgument,
                                                  "primitive wait arguments are invalid");
    }
    eos_skill_primitive_wait_response_v2 response{sizeof(response), 0, {}, {}};
    const auto status = raw_->host->wait_primitive(
        raw_->host->context, operation.id, static_cast<std::uint32_t>(timeout.count()), &response);
    if (status != EOS_SKILL_HOST_OK) {
      auto message = detail::Copy(response.error_message, "primitive wait error");
      return Result<std::optional<Json>>::Failure(
          detail::HostError(status), message ? message.take() : "primitive wait failed");
    }
    if (response.completed == 0) return Result<std::optional<Json>>::Success(std::nullopt);
    if (response.completed != 1) {
      return Result<std::optional<Json>>::Failure(ErrorCode::kMalformedResponse,
                                                  "primitive completion flag is invalid");
    }
    auto acknowledgement =
        detail::ParseObject(response.acknowledgement_json, "primitive acknowledgement");
    if (!acknowledgement) {
      return Result<std::optional<Json>>::Failure(acknowledgement.error().code,
                                                  acknowledgement.error().message);
    }
    return Result<std::optional<Json>>::Success(acknowledgement.take());
  }

  Result<void> cancel_primitive(PrimitiveOperation operation) const {
    if (!EOS_SKILL_HOST_V2_HAS(raw_->host, cancel_primitive) ||
        raw_->host->cancel_primitive == nullptr) {
      return Result<void>::Failure(ErrorCode::kUnavailable,
                                   "primitive cancellation callback unavailable");
    }
    if (operation.id == 0)
      return Result<void>::Failure(ErrorCode::kInvalidArgument,
                                   "primitive operation ID is zero");
    const auto status = raw_->host->cancel_primitive(raw_->host->context, operation.id);
    return status == EOS_SKILL_HOST_OK
               ? Result<void>::Success()
               : Result<void>::Failure(detail::HostError(status),
                                       "primitive cancellation failed");
  }

  Result<Json> read_resource(std::string_view uri) const {
    if (raw_->host->read_resource == nullptr)
      return Result<Json>::Failure(ErrorCode::kUnavailable, "resource callback unavailable");
    if (!detail::Fits(uri))
      return Result<Json>::Failure(ErrorCode::kInvalidArgument, "resource URI is too large");
    eos_skill_resource_request_v2 request{sizeof(request), detail::Borrow(uri)};
    eos_skill_resource_response_v2 response{sizeof(response), {}, {}};
    const auto status = raw_->host->read_resource(raw_->host->context, &request, &response);
    if (status != EOS_SKILL_HOST_OK) {
      auto message = detail::Copy(response.error_message, "resource error");
      return Result<Json>::Failure(detail::HostError(status),
                                   message ? message.take() : "resource read failed");
    }
    return detail::ParseObject(response.value_json, "resource value");
  }

  Result<std::optional<Signal>> receive_signal(std::chrono::milliseconds timeout) const {
    if (raw_->host->receive_signal == nullptr)
      return Result<std::optional<Signal>>::Failure(ErrorCode::kUnavailable,
                                                    "SIGNAL callback unavailable");
    if (timeout.count() < 0 ||
        static_cast<std::uint64_t>(timeout.count()) > std::numeric_limits<std::uint32_t>::max())
      return Result<std::optional<Signal>>::Failure(ErrorCode::kInvalidArgument,
                                                    "SIGNAL timeout is out of range");
    eos_skill_signal_v2 signal{sizeof(signal), {}, {}};
    const auto status = raw_->host->receive_signal(
        raw_->host->context, static_cast<std::uint32_t>(timeout.count()), &signal);
    if (status == EOS_SKILL_SIGNAL_TIMEOUT)
      return Result<std::optional<Signal>>::Success(std::nullopt);
    if (status == EOS_SKILL_SIGNAL_CLOSED)
      return Result<std::optional<Signal>>::Failure(ErrorCode::kClosed, "SIGNAL stream closed");
    if (status != EOS_SKILL_SIGNAL_RECEIVED)
      return Result<std::optional<Signal>>::Failure(
          status == EOS_SKILL_SIGNAL_RECEIVE_INVALID_ARGUMENT ? ErrorCode::kInvalidArgument
                                                              : ErrorCode::kInternal,
          "SIGNAL receive failed");
    auto name = detail::Copy(signal.name, "SIGNAL name");
    if (!name)
      return Result<std::optional<Signal>>::Failure(name.error().code, name.error().message);
    auto payload = detail::ParseObject(signal.payload_json, "SIGNAL payload");
    if (!payload)
      return Result<std::optional<Signal>>::Failure(payload.error().code, payload.error().message);
    return Result<std::optional<Signal>>::Success(Signal{name.take(), payload.take()});
  }

  Result<void> emit_signal(std::string_view name, const Json& payload) const {
    if (raw_->host->emit_signal == nullptr)
      return Result<void>::Failure(ErrorCode::kUnavailable, "SIGNAL callback unavailable");
    if (!detail::Fits(name))
      return Result<void>::Failure(ErrorCode::kInvalidArgument, "SIGNAL name is too large");
    auto encoded = detail::EncodeObject(payload, "SIGNAL payload");
    if (!encoded) return Result<void>::Failure(encoded.error().code, encoded.error().message);
    eos_skill_signal_v2 signal{sizeof(signal), detail::Borrow(name),
                               detail::Borrow(encoded.value())};
    const auto status = raw_->host->emit_signal(raw_->host->context, &signal);
    return status == EOS_SKILL_HOST_OK
               ? Result<void>::Success()
               : Result<void>::Failure(detail::HostError(status), "SIGNAL emit failed");
  }

  Result<SourceSample> publish_source(std::string_view topic, std::int64_t timestamp_ns,
                                      const Json& value) const {
    const auto* host = raw_->host;
    if (!EOS_SKILL_HOST_V2_HAS(host, publish_source) || host->publish_source == nullptr)
      return Result<SourceSample>::Failure(ErrorCode::kUnavailable, "source publish unavailable");
    auto encoded = detail::EncodeObject(value, "source value");
    if (!encoded || !detail::Fits(topic))
      return Result<SourceSample>::Failure(ErrorCode::kInvalidArgument, "invalid source sample");
    eos_skill_source_sample_v2 sample{sizeof(sample), detail::Borrow(topic), timestamp_ns, 0,
                                      detail::Borrow(encoded.value())};
    eos_skill_source_response_v2 response{sizeof(response), 0, {sizeof(response.sample)}, {}};
    const auto status = host->publish_source(host->context, &sample, &response);
    return DecodeSource(status, response);
  }

  Result<std::optional<SourceSample>> query_source(std::string_view topic,
      SourceLookup lookup = SourceLookup::kLatest, std::int64_t timestamp_ns = 0) const {
    const auto* host = raw_->host;
    if (!EOS_SKILL_HOST_V2_HAS(host, query_source) || host->query_source == nullptr)
      return Result<std::optional<SourceSample>>::Failure(ErrorCode::kUnavailable,
                                                          "source query unavailable");
    eos_skill_source_query_v2 query{sizeof(query), detail::Borrow(topic),
                                    static_cast<std::uint32_t>(lookup), timestamp_ns};
    eos_skill_source_response_v2 response{sizeof(response), 0, {sizeof(response.sample)}, {}};
    const auto status = host->query_source(host->context, &query, &response);
    if (status != EOS_SKILL_HOST_OK)
      return Result<std::optional<SourceSample>>::Failure(detail::HostError(status),
                                                          "source query failed");
    if (response.has_sample == 0) return Result<std::optional<SourceSample>>::Success(std::nullopt);
    auto decoded = DecodeSource(status, response);
    if (!decoded) return Result<std::optional<SourceSample>>::Failure(decoded.error().code, decoded.error().message);
    return Result<std::optional<SourceSample>>::Success(decoded.take());
  }

  Result<std::optional<SourceSample>> wait_source(std::string_view topic, std::uint64_t after,
                                                  std::chrono::milliseconds timeout) const {
    const auto* host = raw_->host;
    if (!EOS_SKILL_HOST_V2_HAS(host, wait_source) || host->wait_source == nullptr ||
        timeout.count() < 0 || static_cast<std::uint64_t>(timeout.count()) > UINT32_MAX)
      return Result<std::optional<SourceSample>>::Failure(ErrorCode::kUnavailable,
                                                           "source wait unavailable");
    eos_skill_source_response_v2 response{sizeof(response), 0, {sizeof(response.sample)}, {}};
    const auto status = host->wait_source(host->context, detail::Borrow(topic), after,
                                          static_cast<std::uint32_t>(timeout.count()), &response);
    if (status != EOS_SKILL_HOST_OK)
      return Result<std::optional<SourceSample>>::Failure(detail::HostError(status), "source wait failed");
    if (response.has_sample == 0) return Result<std::optional<SourceSample>>::Success(std::nullopt);
    auto decoded = DecodeSource(status, response);
    if (!decoded) return Result<std::optional<SourceSample>>::Failure(decoded.error().code, decoded.error().message);
    return Result<std::optional<SourceSample>>::Success(decoded.take());
  }

  template <typename Inference>
  Result<Json> infer_once(const RetainedFrame& frame,
                          const Json& options = Json::object()) const {
    const auto* host = raw_->host;
    if (!EOS_SKILL_HOST_V2_HAS(host, infer_once) || host->infer_once == nullptr)
      return Result<Json>::Failure(ErrorCode::kUnavailable, "inference callback unavailable");
    const bool has_any_provenance = !frame.source.empty() || frame.source_sequence != 0 ||
                                    frame.source_generation != 0;
    const bool has_exact_provenance = !frame.source.empty() && frame.source_sequence != 0 &&
                                      frame.source_generation != 0;
    const bool has_inline_media = options.is_object() &&
                                  (options.contains("input") || options.contains("depth_input"));
    if constexpr (Inference::authority == EOS_SKILL_INFERENCE_VAD) {
      if (!frame.source.empty() || frame.source_sequence != 0 || frame.source_generation != 0 ||
          !options.is_object() || !options.contains("input"))
        return Result<Json>::Failure(
            ErrorCode::kInvalidArgument,
            "VAD inference requires caller-supplied input and empty source provenance");
    } else if constexpr (Inference::authority == EOS_SKILL_INFERENCE_CHAT_COMPLETION) {
      if (has_any_provenance || !options.is_object() || has_inline_media)
        return Result<Json>::Failure(
            ErrorCode::kInvalidArgument,
            "chat completion requires text options and empty source provenance");
    } else if constexpr (Inference::authority == EOS_SKILL_INFERENCE_VLM) {
      if (!options.is_object() || options.contains("depth_input") ||
          (has_any_provenance && (!has_exact_provenance || has_inline_media)) ||
          (has_exact_provenance && !detail::Fits(frame.source)))
        return Result<Json>::Failure(ErrorCode::kInvalidArgument,
            "VLM inference requires text with optional inline image or one exact retained frame");
    } else if (!options.is_object() || has_any_provenance == has_inline_media ||
               (has_any_provenance && !has_exact_provenance) ||
               (has_exact_provenance && !detail::Fits(frame.source))) {
      return Result<Json>::Failure(ErrorCode::kInvalidArgument,
                                    "vision inference requires either one exact retained frame or inline media");
    }
    auto encoded = detail::EncodeObject(options, "inference options");
    if (!encoded) return Result<Json>::Failure(encoded.error().code, encoded.error().message);
    eos_skill_inference_request_v2 request{sizeof(request), Inference::authority,
                                            detail::Borrow(frame.source), frame.source_sequence,
                                            frame.source_generation,
                                            detail::Borrow(encoded.value())};
    eos_skill_inference_response_v2 response{sizeof(response), {}, {}};
    const auto status = host->infer_once(host->context, &request, &response);
    if (status != EOS_SKILL_HOST_OK) {
      auto message = detail::Copy(response.error_message, "inference error");
      return Result<Json>::Failure(detail::HostError(status),
                                   message ? message.take() : "inference failed");
    }
    return detail::ParseObject(response.result_json, "inference result");
  }

  template <typename Inference>
  Result<Json> infer_once(const Json& options) const {
    return infer_once<Inference>(RetainedFrame{}, options);
  }

  Result<void> checkpoint(std::string_view name,
                          const CheckpointOptions& options = CheckpointOptions()) const {
    const auto* host = raw_->host;
    if (!EOS_SKILL_HOST_V2_HAS(host, emit_checkpoint) || host->emit_checkpoint == nullptr)
      return Result<void>::Failure(ErrorCode::kUnavailable, "checkpoint callback unavailable");
    if (name.size() > detail::kMaximumEventBytes ||
        options.message().size() > detail::kMaximumMessageBytes)
      return Result<void>::Failure(ErrorCode::kInvalidArgument, "checkpoint text is too large");
    if (options.progress() && (!std::isfinite(*options.progress()) || *options.progress() < 0.0 ||
                               *options.progress() > 1.0))
      return Result<void>::Failure(ErrorCode::kInvalidArgument,
                                   "checkpoint progress must be in [0,1]");
    auto encoded = detail::EncodeObject(options.fields(), "checkpoint fields");
    if (!encoded) return Result<void>::Failure(encoded.error().code, encoded.error().message);
    if (encoded.value().size() > detail::kMaximumObservabilityJsonBytes)
      return Result<void>::Failure(ErrorCode::kInvalidArgument, "checkpoint fields are too large");
    eos_skill_checkpoint_v2 checkpoint{sizeof(checkpoint),
                                       detail::Borrow(name),
                                       detail::Borrow(options.message()),
                                       options.progress() ? 1u : 0u,
                                       options.progress().value_or(0.0),
                                       detail::Borrow(encoded.value())};
    const auto status = host->emit_checkpoint(host->context, &checkpoint);
    return status == EOS_SKILL_HOST_OK
               ? Result<void>::Success()
               : Result<void>::Failure(detail::HostError(status), "checkpoint emit failed");
  }

 private:
  static Result<SourceSample> DecodeSource(eos_skill_host_status_v2 status,
                                            const eos_skill_source_response_v2& response) {
    if (status != EOS_SKILL_HOST_OK || response.has_sample != 1)
      return Result<SourceSample>::Failure(detail::HostError(status), "source operation failed");
    auto topic = detail::Copy(response.sample.topic, "source topic");
    auto value = detail::ParseObject(response.sample.value_json, "source value");
    if (!topic || !value) return Result<SourceSample>::Failure(ErrorCode::kMalformedResponse,
                                                                "malformed source response");
    return Result<SourceSample>::Success({topic.take(), response.sample.source_timestamp_ns,
                                         response.sample.sequence, value.take()});
  }
  friend class Logger;
  const eos_skill_execution_v2* raw_;
  std::string command_id_;
  Json arguments_;
  Logger logger_;
};

inline Result<void> Logger::Write(eos_skill_log_level_v2 level, std::string event,
                                  std::string message, Json fields) const {
  const auto* host = execution_->raw_->host;
  if (!EOS_SKILL_HOST_V2_HAS(host, log) || host->log == nullptr)
    return Result<void>::Failure(ErrorCode::kUnavailable, "log callback unavailable");
  if (event.size() > detail::kMaximumEventBytes || message.size() > detail::kMaximumMessageBytes)
    return Result<void>::Failure(ErrorCode::kInvalidArgument, "log text is too large");
  auto encoded = detail::EncodeObject(fields, "log fields");
  if (!encoded) return Result<void>::Failure(encoded.error().code, encoded.error().message);
  if (encoded.value().size() > detail::kMaximumObservabilityJsonBytes)
    return Result<void>::Failure(ErrorCode::kInvalidArgument, "log fields are too large");
  eos_skill_log_record_v2 record{sizeof(record), level, detail::Borrow(event),
                                 detail::Borrow(message), detail::Borrow(encoded.value())};
  const auto status = host->log(host->context, &record);
  return status == EOS_SKILL_HOST_OK
             ? Result<void>::Success()
             : Result<void>::Failure(detail::HostError(status), "log write failed");
}

#define EOS_SKILL_LOG_METHOD(name, value)                                                       \
  inline Result<void> Logger::name(std::string event, std::string message, Json fields) const { \
    return Write(value, std::move(event), std::move(message), std::move(fields));               \
  }
EOS_SKILL_LOG_METHOD(trace, EOS_SKILL_LOG_TRACE)
EOS_SKILL_LOG_METHOD(debug, EOS_SKILL_LOG_DEBUG)
EOS_SKILL_LOG_METHOD(info, EOS_SKILL_LOG_INFO)
EOS_SKILL_LOG_METHOD(warn, EOS_SKILL_LOG_WARN)
EOS_SKILL_LOG_METHOD(error, EOS_SKILL_LOG_ERROR)
EOS_SKILL_LOG_METHOD(critical, EOS_SKILL_LOG_CRITICAL)
#undef EOS_SKILL_LOG_METHOD

using Handler = Outcome (*)(Execution&);

inline void ReportFailure(const eos_skill_execution_v2* execution, std::string_view reason,
                          std::string_view message) noexcept {
  if (execution == nullptr || execution->host == nullptr ||
      !EOS_SKILL_HOST_V2_HAS(execution->host, report_failure) ||
      execution->host->report_failure == nullptr || reason.size() > detail::kMaximumEventBytes ||
      message.size() > detail::kMaximumMessageBytes)
    return;
  const eos_skill_failure_v2 failure{sizeof(failure), detail::Borrow(reason),
                                     detail::Borrow(message)};
  (void)execution->host->report_failure(execution->host->context, &failure);
}

inline eos_skill_execute_status_v2 Run(Handler handler, const eos_skill_execution_v2* execution,
                                       eos_skill_complete_v2 complete,
                                       void* completion_context) noexcept {
  if (handler == nullptr || execution == nullptr || complete == nullptr)
    return EOS_SKILL_EXECUTE_INVALID_ARGUMENT;
  try {
    Execution view(*execution);
    Outcome outcome = handler(view);
    switch (outcome.kind()) {
      case Outcome::Kind::kCancelled:
        return EOS_SKILL_EXECUTE_CANCELLED;
      case Outcome::Kind::kInvalidArgument:
        ReportFailure(execution, outcome.reason(), outcome.message());
        return EOS_SKILL_EXECUTE_INVALID_ARGUMENT;
      case Outcome::Kind::kFailure:
        ReportFailure(execution, outcome.reason(), outcome.message());
        return EOS_SKILL_EXECUTE_FAILED;
      case Outcome::Kind::kSuccess:
      case Outcome::Kind::kRejected: {
        if (!outcome.result().is_object()) return EOS_SKILL_EXECUTE_FAILED;
        const std::string encoded = outcome.result().dump();
        const eos_skill_completion_v2 completion{
            sizeof(completion), outcome.kind() == Outcome::Kind::kSuccess ? 1u : 0u,
            detail::Borrow(outcome.reason()), detail::Borrow(encoded),
            detail::Borrow(outcome.message())};
        return complete(completion_context, &completion) == EOS_SKILL_HOST_OK
                   ? EOS_SKILL_EXECUTE_OK
                   : EOS_SKILL_EXECUTE_FAILED;
      }
    }
  } catch (...) {
    ReportFailure(execution, "skill.exception", "skill handler threw an exception");
    return EOS_SKILL_EXECUTE_FAILED;
  }
  return EOS_SKILL_EXECUTE_FAILED;
}

enum class SignalDirection { kAccepted, kEmitted };
enum class ClaimKind { kDevice, kResource };
enum class ClaimMode { kSharedRead, kExclusiveUse };
enum class SourceAuthority { kRead, kWrite };
enum class InferenceAuthority { kVad, kHuman, kHumanPose, kFaceRecognition, kChatCompletion, kVlm };

class SkillBuilder {
 public:
  SkillBuilder(std::string name, std::string version, std::string description,
               Json parameters_schema, Json returns_schema, Handler handler)
      : name_(std::move(name)),
        version_(std::move(version)),
        description_(std::move(description)),
        parameters_schema_(parameters_schema.dump()),
        returns_schema_(returns_schema.dump()),
        handler_(handler) {}

  SkillBuilder& Primitive(std::string name, std::string version) {
    primitives_.push_back({std::move(name), std::move(version)});
    return *this;
  }
  SkillBuilder& Resource(std::string uri_template, std::string description = {}) {
    resources_.push_back({std::move(uri_template), std::move(description)});
    return *this;
  }
  SkillBuilder& Signal(std::string name, Json schema, SignalDirection direction) {
    signals_.push_back({std::move(name), schema.dump(), direction});
    return *this;
  }
  SkillBuilder& Claim(ClaimKind kind, ClaimMode mode, std::string argument_pointer) {
    claims_.push_back({kind, mode, std::move(argument_pointer)});
    return *this;
  }
  SkillBuilder& Reentrant(bool value = true) {
    reentrant_ = value;
    return *this;
  }
  SkillBuilder& Source(std::string topic, Json schema, SourceAuthority authority) {
    sources_.push_back({std::move(topic), schema.dump(), authority});
    return *this;
  }
  SkillBuilder& Inference(InferenceAuthority authority) {
    inferences_.push_back(authority);
    return *this;
  }

 private:
  friend class Library;
  struct ResourceValue {
    std::string uri_template, description;
  };
  struct SignalValue {
    std::string name, schema;
    SignalDirection direction;
  };
  struct ClaimValue {
    ClaimKind kind;
    ClaimMode mode;
    std::string pointer;
  };
  struct SourceValue { std::string topic, schema; SourceAuthority authority; };
  std::string name_, version_, description_, parameters_schema_, returns_schema_;
  Handler handler_{nullptr};
  std::vector<PrimitiveRef> primitives_;
  std::vector<ResourceValue> resources_;
  std::vector<SignalValue> signals_;
  std::vector<ClaimValue> claims_;
  std::vector<SourceValue> sources_;
  std::vector<InferenceAuthority> inferences_;
  bool reentrant_{false};
};

class Library {
 public:
  Library(const Library&) = delete;
  Library& operator=(const Library&) = delete;
  Library(Library&& other) noexcept { MoveFrom(std::move(other)); }
  Library& operator=(Library&& other) noexcept {
    if (this != &other) MoveFrom(std::move(other));
    return *this;
  }
  const eos_skill_library_v2* abi() const { return &abi_; }

 private:
  friend class LibraryBuilder;
  struct Node {
    explicit Node(SkillBuilder value)
        : name(std::move(value.name_)),
          version(std::move(value.version_)),
          description(std::move(value.description_)),
          parameters_schema(std::move(value.parameters_schema_)),
          returns_schema(std::move(value.returns_schema_)),
          handler(value.handler_),
          primitive_values(std::move(value.primitives_)),
          resource_values(std::move(value.resources_)),
          signal_values(std::move(value.signals_)),
          claim_values(std::move(value.claims_)),
          source_values(std::move(value.sources_)),
          inference_values(std::move(value.inferences_)),
          reentrant(value.reentrant_) {
      for (const auto& item : primitive_values)
        primitives.push_back({sizeof(eos_skill_primitive_reference_v2), detail::Borrow(item.name),
                              detail::Borrow(item.version)});
      for (const auto& item : resource_values)
        resources.push_back({sizeof(eos_skill_resource_v2), detail::Borrow(item.uri_template),
                             detail::Borrow(item.description)});
      for (const auto& item : signal_values)
        signals.push_back({sizeof(eos_skill_signal_descriptor_v2), detail::Borrow(item.name),
                           detail::Borrow(item.schema),
                           item.direction == SignalDirection::kAccepted
                               ? EOS_SKILL_SIGNAL_ACCEPTED
                               : EOS_SKILL_SIGNAL_EMITTED});
      for (const auto& item : claim_values)
        claims.push_back(
            {sizeof(eos_skill_claim_template_v2),
             item.kind == ClaimKind::kDevice ? EOS_SKILL_CLAIM_DEVICE : EOS_SKILL_CLAIM_RESOURCE,
             item.mode == ClaimMode::kSharedRead ? EOS_SKILL_CLAIM_SHARED_READ
                                                 : EOS_SKILL_CLAIM_EXCLUSIVE_USE,
              detail::Borrow(item.pointer)});
      for (const auto& item : source_values)
        sources.push_back({sizeof(eos_skill_source_descriptor_v2), detail::Borrow(item.topic),
                           detail::Borrow(item.schema), item.authority == SourceAuthority::kRead
                                ? EOS_SKILL_SOURCE_READ : EOS_SKILL_SOURCE_WRITE});
      for (const auto item : inference_values) {
        eos_skill_inference_authority_v2 authority = EOS_SKILL_INFERENCE_VAD;
        switch (item) {
          case InferenceAuthority::kVad: authority = EOS_SKILL_INFERENCE_VAD; break;
          case InferenceAuthority::kHuman: authority = EOS_SKILL_INFERENCE_HUMAN; break;
          case InferenceAuthority::kHumanPose: authority = EOS_SKILL_INFERENCE_HUMAN_POSE; break;
          case InferenceAuthority::kFaceRecognition:
            authority = EOS_SKILL_INFERENCE_FACE_RECOGNITION; break;
          case InferenceAuthority::kChatCompletion:
            authority = EOS_SKILL_INFERENCE_CHAT_COMPLETION; break;
          case InferenceAuthority::kVlm:
            authority = EOS_SKILL_INFERENCE_VLM; break;
        }
        inferences.push_back({sizeof(eos_skill_inference_descriptor_v2), authority});
      }
      descriptor = {sizeof(descriptor),
                    detail::Borrow(name),
                    detail::Borrow(version),
                    detail::Borrow(description),
                    detail::Borrow(parameters_schema),
                    detail::Borrow(returns_schema),
                    static_cast<std::uint32_t>(primitives.size()),
                    primitives.empty() ? nullptr : primitives.data(),
                    static_cast<std::uint32_t>(resources.size()),
                    resources.empty() ? nullptr : resources.data(),
                    static_cast<std::uint32_t>(signals.size()),
                    signals.empty() ? nullptr : signals.data(),
                    static_cast<std::uint32_t>(claims.size()),
                    claims.empty() ? nullptr : claims.data(),
                    reentrant ? 1u : 0u,
                    this,
                     &Execute,
                     static_cast<std::uint32_t>(sources.size()),
                      sources.empty() ? nullptr : sources.data(),
                      static_cast<std::uint32_t>(inferences.size()),
                      inferences.empty() ? nullptr : inferences.data()};
    }
    static eos_skill_execute_status_v2 Execute(void* context,
                                               const eos_skill_execution_v2* execution,
                                               eos_skill_complete_v2 complete,
                                               void* completion_context) {
      auto* node = static_cast<Node*>(context);
      return node == nullptr ? EOS_SKILL_EXECUTE_INVALID_ARGUMENT
                             : Run(node->handler, execution, complete, completion_context);
    }
    std::string name, version, description, parameters_schema, returns_schema;
    Handler handler;
    std::vector<PrimitiveRef> primitive_values;
    std::vector<SkillBuilder::ResourceValue> resource_values;
    std::vector<SkillBuilder::SignalValue> signal_values;
    std::vector<SkillBuilder::ClaimValue> claim_values;
    std::vector<SkillBuilder::SourceValue> source_values;
    std::vector<InferenceAuthority> inference_values;
    bool reentrant;
    std::vector<eos_skill_primitive_reference_v2> primitives;
    std::vector<eos_skill_resource_v2> resources;
    std::vector<eos_skill_signal_descriptor_v2> signals;
    std::vector<eos_skill_claim_template_v2> claims;
    std::vector<eos_skill_source_descriptor_v2> sources;
    std::vector<eos_skill_inference_descriptor_v2> inferences;
    eos_skill_descriptor_v2 descriptor{};
  };

  Library(std::string name, std::string version, std::string build_id,
          std::vector<SkillBuilder> skills)
      : name_(std::move(name)), version_(std::move(version)), build_id_(std::move(build_id)) {
    nodes_.reserve(skills.size());
    descriptors_.reserve(skills.size());
    for (auto& skill : skills) nodes_.push_back(std::make_unique<Node>(std::move(skill)));
    Rebind();
  }
  void MoveFrom(Library&& other) {
    name_ = std::move(other.name_);
    version_ = std::move(other.version_);
    build_id_ = std::move(other.build_id_);
    nodes_ = std::move(other.nodes_);
    descriptors_ = std::move(other.descriptors_);
    Rebind();
  }
  void Rebind() {
    descriptors_.clear();
    descriptors_.reserve(nodes_.size());
    for (const auto& node : nodes_) descriptors_.push_back(node->descriptor);
    abi_ = {EOS_SKILL_ABI_VERSION_V2,
            sizeof(abi_),
            detail::Borrow(name_),
            detail::Borrow(version_),
             detail::Borrow(build_id_),
             static_cast<std::uint32_t>(descriptors_.size()),
             descriptors_.empty() ? nullptr : descriptors_.data(),
             sizeof(eos_skill_descriptor_v2)};
  }
  std::string name_, version_, build_id_;
  std::vector<std::unique_ptr<Node>> nodes_;
  std::vector<eos_skill_descriptor_v2> descriptors_;
  eos_skill_library_v2 abi_{};
};

class LibraryBuilder {
 public:
  LibraryBuilder(std::string name, std::string version, std::string build_id = {})
      : name_(std::move(name)), version_(std::move(version)), build_id_(std::move(build_id)) {}
  LibraryBuilder& Skill(SkillBuilder skill) {
    skills_.push_back(std::move(skill));
    return *this;
  }
  Library Build() {
    return Library(std::move(name_), std::move(version_), std::move(build_id_), std::move(skills_));
  }

 private:
  std::string name_, version_, build_id_;
  std::vector<SkillBuilder> skills_;
};

}  // namespace eos::skill

/// BUILDER_FUNCTION must be a no-argument function returning eos::skill::Library.
#define EOS_SKILL_EXPORT_LIBRARY(BUILDER_FUNCTION)                                    \
  extern "C" EOS_SKILL_EXPORT const eos_skill_library_v2* eos_skill_library_query_v2( \
      uint32_t host_abi_version) {                                                    \
    if (host_abi_version != EOS_SKILL_ABI_VERSION_V2) return nullptr;                 \
    static const ::eos::skill::Library library = BUILDER_FUNCTION();                  \
    return library.abi();                                                             \
  }

#endif  // EOS_SKILL_SDK_HPP_

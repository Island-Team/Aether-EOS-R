#include "src/eos/runtime/models/runtime.h"

#include <algorithm>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <nlohmann/json-schema.hpp>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <toml++/toml.hpp>
#include <utility>

#include "src/eos/lib/log.h"
#include "src/eos/runtime/models/grpc_client.h"

namespace eos::runtime::models {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

constexpr std::size_t kModelWorkerCount = 2;
constexpr std::size_t kMaxQueuedModelCalls = 8;
constexpr std::size_t kHealthWorkerCount = 2;

std::shared_ptr<spdlog::logger>& ModelsLogger() {
  static auto logger = eos::lib::GetLogger("models");
  return logger;
}

class CanonicalAdapter final : public Adapter {
 public:
  nlohmann::json PrepareRequest(const ModelConfig&, const nlohmann::json& request) const override {
    return request;
  }
  nlohmann::json DecodeResponse(const ModelConfig&, std::string_view,
                                std::string_view body) const override {
    return nlohmann::json::parse(body);
  }
  void NormalizeResponse(const ModelConfig&, nlohmann::json*) const override {}
};

const char* CallStatusName(CallResult::Status status) {
  switch (status) {
    case CallResult::Status::kCompleted:
      return "completed";
    case CallResult::Status::kInvalidArgument:
      return "invalid_argument";
    case CallResult::Status::kNotFound:
      return "not_found";
    case CallResult::Status::kUnavailable:
      return "unavailable";
    case CallResult::Status::kDeadlineExceeded:
      return "deadline_exceeded";
    case CallResult::Status::kRemoteError:
      return "remote_error";
    case CallResult::Status::kProtocolError:
      return "protocol_error";
    case CallResult::Status::kInternal:
      return "internal";
  }
  return "internal";
}

const char* WireFormatName(WireFormat format) {
  switch (format) {
    case WireFormat::kJson:
      return "json";
    case WireFormat::kMultipart:
      return "multipart";
  }
  return "unknown";
}

const char* HealthStatusName(HealthStatus status) {
  switch (status) {
    case HealthStatus::kChecking:
      return "checking";
    case HealthStatus::kReady:
      return "ready";
    case HealthStatus::kDegraded:
      return "degraded";
    case HealthStatus::kUnavailable:
      return "unavailable";
  }
  return "unavailable";
}

[[noreturn]] void Fail(const std::string& path, const std::string& what);

HttpMethod ParseHttpMethod(const std::string& value, const std::string& path, std::size_t index) {
  if (value == "post") return HttpMethod::kPost;
  if (value == "get") return HttpMethod::kGet;
  if (value == "delete") return HttpMethod::kDelete;
  Fail(path, "model[" + std::to_string(index) + "] has unknown http_method '" + value + "'");
}

struct Endpoint {
  std::string host;
  std::uint16_t port{80};
  std::string target;
  std::string authority;
};

[[noreturn]] void Fail(const std::string& path, const std::string& what) {
  throw std::runtime_error("models '" + path + "': " + what);
}

bool ValidName(const std::string& value) {
  if (value.empty()) return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return std::isalnum(c) != 0 || c == '.' || c == '_' || c == '-';
  });
}

Capability ParseCapability(const std::string& value, const std::string& path, std::size_t index) {
  if (value == "audio.vad") return Capability::kVad;
  if (value == "audio.transcription") return Capability::kAsr;
  if (value == "audio.synthesis") return Capability::kTts;
  if (value == "vision.object_detection") return Capability::kDetection;
  if (value == "vision.human") return Capability::kHuman;
  if (value == "vision.human_pose") return Capability::kHumanPose;
  if (value == "vision.pose") return Capability::kPose;
  if (value == "vision.posture") return Capability::kPosture;
  if (value == "vision.posture_classification") return Capability::kPostureClassification;
  if (value == "vision.vlm") return Capability::kVlm;
  if (value == "vision.face") return Capability::kFace;
  if (value == "arm.grasp_plan") return Capability::kArmGraspPlan;
  if (value == "text.chat_completion") return Capability::kChatCompletion;
  Fail(path, "model[" + std::to_string(index) + "] has unknown capability '" + value + "'");
}

std::string ContractStem(Capability capability) {
  switch (capability) {
    case Capability::kVad:
      return "vad";
    case Capability::kAsr:
      return "asr";
    case Capability::kTts:
      return "tts";
    case Capability::kDetection:
      return "detection";
    case Capability::kHuman:
      return "human";
    case Capability::kHumanPose:
      return "human_pose";
    case Capability::kPose:
      return "pose";
    case Capability::kPosture:
      return "posture";
    case Capability::kPostureClassification:
      return "gemma_posture";
    case Capability::kVlm:
      return "vlm";
    case Capability::kFace:
      return "face";
    case Capability::kArmGraspPlan:
      return "grasp_plan";
    case Capability::kChatCompletion:
      return "chat_completion";
  }
  throw std::logic_error("unhandled model capability");
}

void ModelContentCheck(const std::string& encoding, const std::string& /*media_type*/,
                       const nlohmann::json& instance) {
  if (encoding != "base64" || !instance.is_string()) return;
  const std::string value = instance.get<std::string>();
  const std::size_t padding = static_cast<std::size_t>(std::count(value.begin(), value.end(), '='));
  const std::size_t first_padding = value.find('=');
  const bool alphabet = std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return std::isalnum(c) != 0 || c == '+' || c == '/' || c == '=';
  });
  if (!alphabet || value.size() % 4 != 0 || padding > 2 ||
      (first_padding != std::string::npos && first_padding != value.size() - padding)) {
    throw std::runtime_error("value is not valid base64");
  }
}

class ModelSchemaStore final {
 public:
  ModelSchemaStore() {
    const std::filesystem::path source(EOS_MODEL_SCHEMA_SOURCE_DIR);
    const std::filesystem::path installed(EOS_MODEL_SCHEMA_INSTALL_DIR);
    const std::filesystem::path root = std::filesystem::is_directory(source) ? source : installed;
    if (!std::filesystem::is_directory(root)) {
      throw std::runtime_error("private model schema directory does not exist: " + root.string());
    }
    for (const char* stem : {"vad", "asr", "tts", "detection", "human", "human_pose", "pose",
                             "posture", "gemma_posture", "vlm", "face", "grasp_plan", "chat_completion"}) {
      for (const char* direction : {"request", "response"}) {
        const std::string name = std::string(stem) + "_" + direction + ".v1.json";
        const std::filesystem::path path = root / name;
        std::ifstream input(path);
        if (!input) throw std::runtime_error("cannot read private model schema: " + path.string());
        nlohmann::json schema;
        try {
          input >> schema;
          auto validator = std::make_unique<nlohmann::json_schema::json_validator>(
              nullptr, nlohmann::json_schema::default_string_format_check, ModelContentCheck);
          validator->set_root_schema(schema);
          validators_.emplace(name, std::move(validator));
        } catch (const std::exception& error) {
          throw std::runtime_error("invalid private model schema '" + path.string() +
                                   "': " + error.what());
        }
      }
    }
  }

  void Validate(const std::string& contract, const nlohmann::json& instance) const {
    const std::string name = std::filesystem::path(contract).filename().string();
    const auto found = validators_.find(name);
    if (found == validators_.end()) {
      throw std::runtime_error("unknown private model schema '" + contract + "'");
    }
    found->second->validate(instance);
  }

 private:
  std::map<std::string, std::unique_ptr<nlohmann::json_schema::json_validator>> validators_;
};

Endpoint ParseEndpoint(const std::string& value, const std::string& error_prefix) {
  constexpr std::string_view kHttpScheme = "http://";
  const bool is_http = value.rfind(kHttpScheme, 0) == 0;
  if (!is_http) {
    throw std::runtime_error(error_prefix + " endpoint must use http://");
  }
  const std::string remainder = value.substr(kHttpScheme.size());
  if (remainder.find('#') != std::string::npos) {
    throw std::runtime_error(error_prefix + " endpoint must not contain a fragment");
  }
  const std::size_t target_start = remainder.find_first_of("/?");
  const std::string authority = remainder.substr(0, target_start);
  std::string target = target_start == std::string::npos ? "/" : remainder.substr(target_start);
  if (!target.empty() && target.front() == '?') target.insert(target.begin(), '/');
  if (authority.empty() || authority.find('@') != std::string::npos ||
      authority.find_first_of(" 	\r\n\\") != std::string::npos) {
    throw std::runtime_error(error_prefix + " endpoint is not a valid HTTP URL");
  }

  Endpoint endpoint;
  endpoint.authority = authority;
  endpoint.target = std::move(target);
  std::string port_text = "80";
  if (authority.front() == '[') {
    const std::size_t close = authority.find(']');
    if (close == std::string::npos || close == 1) {
      throw std::runtime_error(error_prefix + " endpoint has an invalid IPv6 host");
    }
    endpoint.host = authority.substr(1, close - 1);
    if (close + 1 < authority.size()) {
      if (authority[close + 1] != ':' || close + 2 == authority.size()) {
        throw std::runtime_error(error_prefix + " endpoint has an invalid port");
      }
      port_text = authority.substr(close + 2);
    }
  } else {
    const std::size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
      endpoint.host = authority.substr(0, colon);
      port_text = authority.substr(colon + 1);
    } else {
      endpoint.host = authority;
    }
  }
  if (endpoint.host.empty() || port_text.empty() ||
      !std::all_of(port_text.begin(), port_text.end(),
                   [](unsigned char c) { return std::isdigit(c) != 0; })) {
    throw std::runtime_error(error_prefix + " endpoint has an invalid host or port");
  }
  unsigned long parsed_port = 0;
  try {
    parsed_port = std::stoul(port_text);
  } catch (const std::exception&) {
    throw std::runtime_error(error_prefix + " endpoint has an invalid port");
  }
  if (parsed_port == 0 || parsed_port > 65535) {
    throw std::runtime_error(error_prefix + " endpoint port must be in (0, 65535]");
  }
  beast::error_code address_error;
  asio::ip::make_address(endpoint.host, address_error);
  if (address_error) {
    throw std::runtime_error(error_prefix + " endpoint host must be a numeric IP address");
  }
  bool invalid_target = false;
  for (std::size_t i = 0; i < endpoint.target.size(); ++i) {
    const unsigned char c = endpoint.target[i];
    if (c <= 0x20 || c >= 0x7F || c == '\\') {
      invalid_target = true;
      break;
    }
    if (c == '%' && (i + 2 >= endpoint.target.size() ||
                     std::isxdigit(static_cast<unsigned char>(endpoint.target[i + 1])) == 0 ||
                     std::isxdigit(static_cast<unsigned char>(endpoint.target[i + 2])) == 0)) {
      invalid_target = true;
      break;
    }
    if (c == '%') i += 2;
  }
  if (invalid_target) {
    throw std::runtime_error(error_prefix + " endpoint target contains an unescaped character");
  }
  endpoint.port = static_cast<std::uint16_t>(parsed_port);
  return endpoint;
}

Endpoint ParseModelEndpoint(const ModelConfig& model, const std::string& error_prefix) {
  if (model.transport == Transport::kGrpc) {
    if (model.endpoint.rfind("unix:/", 0) != 0 &&
        model.endpoint.rfind("127.0.0.1:", 0) != 0 &&
        model.endpoint.rfind("[::1]:", 0) != 0) {
      throw std::runtime_error(error_prefix + " gRPC endpoint must use unix:/ or loopback TCP");
    }
    return {};
  }
  if (model.transport == Transport::kHttp) return ParseEndpoint(model.endpoint, error_prefix);
  constexpr std::string_view kWebSocketScheme = "ws://";
  if (model.endpoint.rfind(kWebSocketScheme, 0) != 0) {
    throw std::runtime_error(error_prefix + " endpoint must use ws://");
  }
  return ParseEndpoint("http://" + model.endpoint.substr(kWebSocketScheme.size()), error_prefix);
}

std::vector<unsigned char> DecodeBase64(const std::string& input) {
  static constexpr signed char kInvalid = -1;
  static constexpr signed char kPadding = -2;
  signed char table[256];
  std::fill(std::begin(table), std::end(table), kInvalid);
  const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  for (std::size_t i = 0; i < alphabet.size(); ++i) {
    table[static_cast<unsigned char>(alphabet[i])] = static_cast<signed char>(i);
  }
  table[static_cast<unsigned char>('=')] = kPadding;

  if (input.empty() || input.size() % 4 != 0) {
    throw std::runtime_error("input.data_b64 is not valid base64");
  }
  std::vector<unsigned char> output;
  output.reserve(input.size() / 4 * 3);
  for (std::size_t offset = 0; offset < input.size(); offset += 4) {
    signed char values[4];
    for (int i = 0; i < 4; ++i) {
      values[i] = table[static_cast<unsigned char>(input[offset + i])];
      if (values[i] == kInvalid) throw std::runtime_error("input.data_b64 is not valid base64");
    }
    const bool last = offset + 4 == input.size();
    if (values[0] < 0 || values[1] < 0 || (!last && (values[2] < 0 || values[3] < 0)) ||
        (values[2] == kPadding && values[3] != kPadding)) {
      throw std::runtime_error("input.data_b64 is not valid base64");
    }
    output.push_back(static_cast<unsigned char>((values[0] << 2) | (values[1] >> 4)));
    if (values[2] != kPadding) {
      output.push_back(static_cast<unsigned char>(((values[1] & 0x0F) << 4) | (values[2] >> 2)));
      if (values[3] != kPadding) {
        output.push_back(static_cast<unsigned char>(((values[2] & 0x03) << 6) | values[3]));
      }
    }
  }
  return output;
}

void CheckMultipartToken(const std::string& value, const char* name) {
  if (value.empty() || std::any_of(value.begin(), value.end(), [](unsigned char c) {
        return c < 0x20 || c >= 0x7F || c == '"' || c == '\\';
      })) {
    throw std::runtime_error(std::string(name) + " is not safe for multipart encoding");
  }
}

bool ValidMediaType(const std::string& value) {
  const std::size_t slash = value.find('/');
  if (slash == 0 || slash == std::string::npos || slash + 1 == value.size()) return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return std::isalnum(c) != 0 || c == '/' || c == '.' || c == '+' || c == '-';
  });
}

std::string ScalarText(const nlohmann::json& value, const std::string& name) {
  if (value.is_string()) return value.get<std::string>();
  if (value.is_boolean() || value.is_number()) return value.dump();
  throw std::runtime_error("multipart field '" + name + "' must be a string, number, or boolean");
}

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

std::optional<double> VadInputDuration(const nlohmann::json& request) {
  try {
    const std::vector<unsigned char> bytes =
        DecodeBase64(request.at("input").at("data_b64").get<std::string>());
    if (bytes.size() < 12 || std::string(bytes.begin(), bytes.begin() + 4) != "RIFF" ||
        std::string(bytes.begin() + 8, bytes.begin() + 12) != "WAVE") {
      return std::nullopt;
    }
    const std::size_t riff_end = static_cast<std::size_t>(ReadLe32(bytes, 4)) + 8;
    if (riff_end > bytes.size()) return std::nullopt;
    std::uint32_t bytes_per_second = 0;
    std::size_t data_bytes = 0;
    for (std::size_t offset = 12; offset + 8 <= riff_end;) {
      const std::string id(bytes.begin() + offset, bytes.begin() + offset + 4);
      const std::size_t size = ReadLe32(bytes, offset + 4);
      const std::size_t data = offset + 8;
      if (size > riff_end - data) return std::nullopt;
      if (id == "fmt " && size >= 16 && ReadLe16(bytes, data) == 1) {
        bytes_per_second = ReadLe32(bytes, data + 8);
      } else if (id == "data") {
        data_bytes += size;
      }
      if (data + size + size % 2 > riff_end) return std::nullopt;
      offset = data + size + size % 2;
    }
    if (bytes_per_second == 0 || data_bytes == 0) return std::nullopt;
    return static_cast<double>(data_bytes) / bytes_per_second;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::string BuildMultipart(const ModelConfig& model, const nlohmann::json& request,
                           std::string* content_type) {
  if (!request.is_object() || !request.contains("input") || !request.at("input").is_object()) {
    throw std::runtime_error("multipart request requires an input object");
  }
  const nlohmann::json& input = request.at("input");
  if (!input.contains("data_b64") || !input.at("data_b64").is_string()) {
    throw std::runtime_error("multipart request input.data_b64 must be a string");
  }
  const bool audio = model.capability == Capability::kVad || model.capability == Capability::kAsr;
  const std::string filename = input.value("filename", audio ? "audio.wav" : "image.jpg");
  const std::string media_type = input.value("media_type", audio ? "audio/wav" : "image/jpeg");
  for (const auto& [field, value] : std::vector<std::pair<std::string, std::string>>{
           {"filename", filename}, {"media_type", media_type}}) {
    if (!input.contains(field)) {
      eos::lib::LogOnce(ModelsLogger(), spdlog::level::warn,
                        "model-default:" + model.id + ":input." + field,
                        "model_optional_field_defaulted",
                        {{"model_id", model.id},
                         {"capability", std::string(CapabilityName(model.capability))},
                         {"field", "input." + field},
                         {"default", value}});
    }
  }
  CheckMultipartToken(model.file_field, "file_field");
  CheckMultipartToken(filename, "input.filename");
  if (!ValidMediaType(media_type)) {
    throw std::runtime_error("input.media_type is not safe for multipart encoding");
  }
  const std::vector<unsigned char> bytes = DecodeBase64(input.at("data_b64").get<std::string>());
  std::vector<unsigned char> depth_bytes;
  std::string depth_filename;
  std::string depth_media_type;
  if (request.contains("depth_input")) {
    const nlohmann::json& depth = request.at("depth_input");
    if (!depth.is_object()) {
      throw std::runtime_error("multipart request depth_input must be an object");
    }
    for (const char* key : {"data_b64", "filename", "media_type"}) {
      if (!depth.contains(key) || !depth.at(key).is_string()) {
        throw std::runtime_error(std::string("multipart request depth_input.") + key +
                                 " must be a string");
      }
    }
    depth_filename = depth.at("filename").get<std::string>();
    depth_media_type = depth.at("media_type").get<std::string>();
    CheckMultipartToken(depth_filename, "depth_input.filename");
    if (!ValidMediaType(depth_media_type)) {
      throw std::runtime_error("depth_input.media_type is not safe for multipart encoding");
    }
    depth_bytes = DecodeBase64(depth.at("data_b64").get<std::string>());
  }
  std::map<std::string, std::string> fields(model.fields.begin(), model.fields.end());
  if (model.capability == Capability::kVad) {
    fields.try_emplace("sample_rate_hz", "16000");
    fields.try_emplace("channels", "1");
  }
  for (const auto& [key, value] : input.items()) {
    if (key != "data_b64" && key != "filename" && key != "media_type") {
      fields[key] = ScalarText(value, key);
    }
  }
  for (const auto& [key, value] : request.items()) {
    if (key != "input" && key != "depth_input") {
      fields[key] = ScalarText(value, key);
    }
  }

  std::string boundary = "eos-perception-boundary";
  const auto BoundaryCollides = [&] {
    if (std::search(bytes.begin(), bytes.end(), boundary.begin(), boundary.end()) != bytes.end()) {
      return true;
    }
    if (std::search(depth_bytes.begin(), depth_bytes.end(), boundary.begin(), boundary.end()) !=
        depth_bytes.end()) {
      return true;
    }
    return std::any_of(fields.begin(), fields.end(), [&](const auto& field) {
      return field.first.find(boundary) != std::string::npos ||
             field.second.find(boundary) != std::string::npos;
    });
  };
  while (BoundaryCollides()) boundary.push_back('-');

  std::string body;
  for (const auto& [name, value] : fields) {
    CheckMultipartToken(name, "multipart field name");
    body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + name + "\"\r\n\r\n" +
            value + "\r\n";
  }
  body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + model.file_field +
          "\"; filename=\"" + filename + "\"\r\nContent-Type: " + media_type + "\r\n\r\n";
  body.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  body += "\r\n";
  if (!depth_bytes.empty()) {
    body += "--" + boundary +
            "\r\nContent-Disposition: form-data; name=\"depth_file\"; filename=\"" +
            depth_filename + "\"\r\nContent-Type: " + depth_media_type + "\r\n\r\n";
    body.append(reinterpret_cast<const char*>(depth_bytes.data()), depth_bytes.size());
    body += "\r\n";
  }
  body += "--" + boundary + "--\r\n";
  *content_type = "multipart/form-data; boundary=" + boundary;
  return body;
}

nlohmann::json BuildJson(const ModelConfig& model, const nlohmann::json& request) {
  nlohmann::json payload = request;
  for (const auto& [key, value] : model.fields) {
    if (!payload.contains(key)) payload[key] = value;
  }
  return payload;
}

CallResult Post(const ModelConfig& model, const Endpoint& endpoint, const nlohmann::json& request,
                const Adapter& adapter, const CancelRequested& cancel_requested) {
  CallResult result;
  try {
    if (cancel_requested && cancel_requested()) {
      result.status = CallResult::Status::kUnavailable;
      result.message = "model '" + model.id + "' request was cancelled";
      return result;
    }
    std::string content_type = "application/json";
    std::string body;
    if (model.http_method != HttpMethod::kGet) {
      if (model.wire_format == WireFormat::kMultipart) {
        body = BuildMultipart(model, request, &content_type);
      } else {
        body = BuildJson(model, request).dump();
      }
    }
    if (body.size() > model.max_request_bytes) {
      result.status = CallResult::Status::kInvalidArgument;
      result.message = "model '" + model.id + "' request exceeds configured byte limit";
      return result;
    }

    asio::io_context io;
    beast::tcp_stream stream(io);

    const http::verb verb = model.http_method == HttpMethod::kGet      ? http::verb::get
                            : model.http_method == HttpMethod::kDelete ? http::verb::delete_
                                                                       : http::verb::post;
    http::request<http::string_body> outbound{verb, endpoint.target, 11};
    outbound.set(http::field::host, endpoint.authority);
    outbound.set(http::field::user_agent, "island-eos-perception/1.0");
    if (!body.empty()) outbound.set(http::field::content_type, content_type);
    if (!model.api_key_env.empty()) {
      const char* key = std::getenv(model.api_key_env.c_str());
      if (key == nullptr || *key == '\0') {
        result.status = CallResult::Status::kUnavailable;
        result.message =
            "model '" + model.id + "' requires environment variable " + model.api_key_env;
        return result;
      }
      outbound.set(http::field::authorization, std::string("Bearer ") + key);
    }
    outbound.body() = std::move(body);
    outbound.prepare_payload();

    beast::flat_buffer buffer;
    http::response_parser<http::string_body> parser;
    parser.body_limit(model.max_response_bytes);
    asio::steady_timer deadline(io);
    deadline.expires_after(model.timeout);
    asio::steady_timer cancellation(io);
    asio::steady_timer reconnect(io);
    beast::error_code operation_error;
    bool complete = false;
    bool timed_out = false;
    bool cancelled = false;
    const auto Finish = [&](beast::error_code error) {
      if (complete) return;
      complete = true;
      operation_error = error;
      deadline.cancel();
      cancellation.cancel();
      reconnect.cancel();
    };
    deadline.async_wait([&](beast::error_code error) {
      if (error || complete) return;
      timed_out = true;
      complete = true;
      beast::error_code ignored;
      cancellation.cancel();
      reconnect.cancel();
      stream.socket().cancel(ignored);
      stream.socket().close(ignored);
    });
    std::function<void()> poll_cancellation;
    if (cancel_requested) {
      poll_cancellation = [&] {
        cancellation.expires_after(std::chrono::milliseconds(10));
        cancellation.async_wait([&](beast::error_code error) {
          if (error || complete) return;
          if (!cancel_requested()) {
            poll_cancellation();
            return;
          }
          cancelled = true;
          complete = true;
          beast::error_code ignored;
          deadline.cancel();
          reconnect.cancel();
          stream.socket().cancel(ignored);
          stream.socket().close(ignored);
        });
      };
      poll_cancellation();
    }
    const tcp::endpoint remote(asio::ip::make_address(endpoint.host), endpoint.port);
    std::size_t connection_attempts = 0;
    std::function<void()> connect;
    connect = [&] {
      ++connection_attempts;
      stream.async_connect(remote, [&](beast::error_code connect_error) {
        if (complete) return;
        if (connect_error) {
          beast::error_code ignored;
          stream.socket().close(ignored);
          reconnect.expires_after(std::chrono::milliseconds(100));
          reconnect.async_wait([&](beast::error_code error) {
            if (!error && !complete) connect();
          });
          return;
        }
        eos::lib::LogEvent(ModelsLogger(), spdlog::level::debug, "model_connected",
                           {{"model_id", model.id},
                            {"endpoint", endpoint.authority},
                            {"attempts", connection_attempts}});
        http::async_write(stream, outbound, [&](beast::error_code write_error, std::size_t) {
          if (complete) return;
          if (write_error) return Finish(write_error);
          http::async_read(stream, buffer, parser, [&](beast::error_code read_error, std::size_t) {
            if (complete) return;
            Finish(read_error);
          });
        });
      });
    };
    connect();
    io.run();
    if (cancelled) {
      result.status = CallResult::Status::kUnavailable;
      result.message = "model '" + model.id + "' request was cancelled";
      return result;
    }
    if (timed_out) {
      result.status = CallResult::Status::kDeadlineExceeded;
      result.message = "model '" + model.id + "' request exceeded " +
                       std::to_string(model.timeout.count()) + " ms";
      return result;
    }
    if (operation_error) {
      const beast::error_code http_category = http::error::bad_status;
      result.status = operation_error.category() == http_category.category()
                          ? CallResult::Status::kProtocolError
                          : CallResult::Status::kUnavailable;
      result.message = "model '" + model.id + "' request failed: " + operation_error.message();
      return result;
    }
    const auto response = parser.release();
    result.http_status = response.result_int();
    beast::error_code ignored;
    stream.socket().shutdown(tcp::socket::shutdown_both, ignored);

    if (response.body().size() > model.max_response_bytes) {
      result.status = CallResult::Status::kProtocolError;
      result.message = "model '" + model.id + "' response exceeds configured byte limit";
      return result;
    }

    if (result.http_status < 200 || result.http_status >= 300) {
      result.status = CallResult::Status::kRemoteError;
      result.message =
          "model '" + model.id + "' returned HTTP " + std::to_string(result.http_status);
      try {
        result.response = nlohmann::json::parse(response.body());
      } catch (const nlohmann::json::parse_error&) {
        result.response = response.body();
      }
      return result;
    }
    try {
      const beast::string_view header = response[http::field::content_type];
      result.response = adapter.DecodeResponse(
          model, std::string_view(header.data(), header.size()), response.body());
    } catch (const std::exception& error) {
      result.status = CallResult::Status::kProtocolError;
      result.message = "model '" + model.id + "' returned an invalid response: " + error.what();
      return result;
    }
    result.status = CallResult::Status::kCompleted;
    return result;
  } catch (const beast::system_error& error) {
    result.status = error.code() == beast::error::timeout ? CallResult::Status::kDeadlineExceeded
                                                          : CallResult::Status::kUnavailable;
    result.message = "model '" + model.id + "' request failed: " + error.code().message();
    return result;
  } catch (const std::exception& error) {
    result.status = CallResult::Status::kInvalidArgument;
    result.message = "model '" + model.id + "' request failed: " + error.what();
    return result;
  }
}

ModelHealth ProbeHealth(const ModelConfig& model, const Endpoint& endpoint,
                         const Adapter& provider_adapter, CancelRequested cancel_requested) {
  ModelHealth health;
  health.model_id = model.id;
  health.status = HealthStatus::kUnavailable;
  health.checked_at = std::chrono::system_clock::now();

  ModelConfig request_config = model;
  request_config.http_method = HttpMethod::kGet;
  request_config.wire_format = WireFormat::kJson;
  request_config.timeout = model.health_timeout;
  const CanonicalAdapter health_adapter;
  const CallResult result = model.transport == Transport::kGrpc
                                ? GrpcHealth(model, cancel_requested)
                                : Post(request_config, endpoint, nlohmann::json::object(),
                                       health_adapter, std::move(cancel_requested));
  if (result.status != CallResult::Status::kCompleted) {
    health.message = result.message;
    return health;
  }

  try {
    nlohmann::json value = result.response;
    provider_adapter.NormalizeHealthResponse(model, &value);
    if (!value.is_object()) throw std::runtime_error("response must be an object");
    static const std::set<std::string> kHealthKeys = {
        "interface_version", "status", "service", "model", "capabilities", "message"};
    for (const auto& [key, unused] : value.items()) {
      (void)unused;
      if (kHealthKeys.count(key) == 0) {
        throw std::runtime_error("unknown health field '" + key + "'");
      }
    }
    const auto RequiredString = [&](const char* key) -> std::string {
      const auto found = value.find(key);
      if (found == value.end() || !found->is_string() ||
          found->get_ref<const std::string&>().empty()) {
        throw std::runtime_error(std::string("'") + key + "' must be a non-empty string");
      }
      return found->get<std::string>();
    };
    health.interface_version = RequiredString("interface_version");
    if (health.interface_version != "1.0") {
      throw std::runtime_error("unsupported health interface_version '" + health.interface_version +
                               "'");
    }
    const std::string status = RequiredString("status");
    if (status == "ready") {
      health.status = HealthStatus::kReady;
    } else if (status == "degraded") {
      health.status = HealthStatus::kDegraded;
    } else if (status == "unavailable") {
      health.status = HealthStatus::kUnavailable;
    } else {
      throw std::runtime_error("'status' must be ready, degraded, or unavailable");
    }
    const auto ReadIdentity = [&](const char* key, const char* identity_key, std::string* name,
                                  std::string* version) {
      const auto found = value.find(key);
      if (found == value.end() || !found->is_object()) {
        throw std::runtime_error(std::string("'") + key + "' must be an object");
      }
      if (found->size() != 2 || !found->contains(identity_key) || !found->contains("version")) {
        throw std::runtime_error(std::string("'") + key + "' requires only " + identity_key +
                                 " and version");
      }
      const auto name_value = found->find(identity_key);
      const auto version_value = found->find("version");
      if (name_value == found->end() || !name_value->is_string() ||
          name_value->get_ref<const std::string&>().empty() || version_value == found->end() ||
          !version_value->is_string() || version_value->get_ref<const std::string&>().empty()) {
        throw std::runtime_error(std::string("'") + key + "' requires non-empty name and version");
      }
      *name = name_value->get<std::string>();
      *version = version_value->get<std::string>();
    };
    ReadIdentity("service", "name", &health.service_name, &health.service_version);
    ReadIdentity("model", "id", &health.provider_model_id, &health.model_version);
    const auto capabilities = value.find("capabilities");
    if (capabilities == value.end() || !capabilities->is_array() || capabilities->empty()) {
      throw std::runtime_error("'capabilities' must be a non-empty string array");
    }
    std::set<std::string> unique;
    for (const auto& capability : *capabilities) {
      if (!capability.is_string() || capability.get_ref<const std::string&>().empty() ||
          !unique.emplace(capability.get<std::string>()).second) {
        throw std::runtime_error("'capabilities' must contain unique non-empty strings");
      }
      health.capabilities.push_back(capability.get<std::string>());
    }
    if (unique.count(std::string(CapabilityName(model.capability))) == 0) {
      throw std::runtime_error("provider does not advertise configured capability");
    }
    if (!model.provider_model_id.empty() && health.provider_model_id != model.provider_model_id) {
      throw std::runtime_error("provider model identity does not match configuration");
    }
    if (const auto message = value.find("message"); message != value.end()) {
      if (!message->is_string()) throw std::runtime_error("'message' must be a string");
      health.message = message->get<std::string>();
    }
  } catch (const std::exception& error) {
    health = ModelHealth{model.id, HealthStatus::kUnavailable};
    health.checked_at = std::chrono::system_clock::now();
    health.message = std::string("invalid health response: ") + error.what();
  }
  return health;
}

std::string ResponseInvariantError(Capability capability, const nlohmann::json& request,
                                   const nlohmann::json& response) {
  if (capability == Capability::kArmGraspPlan) {
    for (const char* key : {"schema_version", "request_id", "robot_id", "camera_id", "object_id", "source_timestamp_ns"}) {
      if (!request.contains(key) || !response.contains(key) || request.at(key) != response.at(key))
        return std::string("grasp plan response mismatch: ") + key;
    }
    if (response.value("frame_id", std::string()) != "base_link")
      return "grasp plan frame_id must be base_link";
    return {};
  }
  if (capability == Capability::kVad) {
    if (!response.is_object() || !response.contains("speech") ||
        !response.at("speech").is_boolean() || !response.contains("probability") ||
        !response.at("probability").is_number() || !response.contains("segments") ||
        !response.at("segments").is_array()) {
      return {};
    }
    const nlohmann::json& segments = response.at("segments");
    if (response.at("speech").get<bool>() != !segments.empty()) {
      return "speech does not match segment presence";
    }
    double maximum_confidence = 0.0;
    double previous_end = 0.0;
    bool first = true;
    const std::optional<double> input_duration = VadInputDuration(request);
    for (const nlohmann::json& segment : segments) {
      if (!segment.is_object() || !segment.contains("start_s") ||
          !segment.at("start_s").is_number() || !segment.contains("end_s") ||
          !segment.at("end_s").is_number() || !segment.contains("confidence") ||
          !segment.at("confidence").is_number()) {
        return {};
      }
      const double start = segment.at("start_s").get<double>();
      const double end = segment.at("end_s").get<double>();
      if (end <= start) return "segment end_s must exceed start_s";
      if (input_duration.has_value() && end > *input_duration) {
        return "segment end_s exceeds input duration";
      }
      if (!first && start < previous_end) return "segments are not ordered and non-overlapping";
      first = false;
      previous_end = end;
      maximum_confidence = std::max(maximum_confidence, segment.at("confidence").get<double>());
    }
    if (response.at("probability").get<double>() != maximum_confidence) {
      return "probability does not match maximum segment confidence";
    }
    return {};
  }

  if (capability == Capability::kAsr) {
    if (!response.is_object() || !response.contains("segments") ||
        !response.at("segments").is_array()) {
      return {};
    }
    const std::optional<double> duration =
        response.contains("duration_s") && response.at("duration_s").is_number()
            ? std::optional<double>(response.at("duration_s").get<double>())
            : std::nullopt;
    for (std::size_t index = 0; index < response.at("segments").size(); ++index) {
      const auto& segment = response.at("segments").at(index);
      if (!segment.is_object() || !segment.contains("start_s") || !segment.contains("end_s") ||
          !segment.at("start_s").is_number() || !segment.at("end_s").is_number()) {
        continue;
      }
      const double start = segment.at("start_s").get<double>();
      const double end = segment.at("end_s").get<double>();
      if (end < start) return "segments[" + std::to_string(index) + "] ends before it starts";
      if (duration.has_value() && end > *duration) {
        return "segments[" + std::to_string(index) + "] exceeds duration_s";
      }
    }
    return {};
  }

  if (capability == Capability::kFace) {
    if (!response.is_object() || !response.contains("data") || !response.at("data").is_object()) {
      return "face response must contain an object data payload";
    }
    if (!request.is_object() || !request.contains("operation") ||
        !request.at("operation").is_string()) {
      return "face request must contain operation";
    }
    const std::string operation = request.at("operation").get<std::string>();
    if (operation != "recognize") return "face inference supports only recognize";
    if (!response.contains("operation") || response.at("operation") != operation) {
      return "face response operation must match request operation";
    }
    const auto& data = response.at("data");
    if (!data.contains("faces") || !data.at("faces").is_array() || !data.contains("face_count") ||
        !data.at("face_count").is_number_unsigned() ||
        data.at("faces").size() != data.at("face_count").get<std::size_t>()) {
      return "recognition response data must contain matching faces and face_count";
    }
    for (std::size_t index = 0; index < data.at("faces").size(); ++index) {
      const auto& face = data.at("faces").at(index);
      if (!face.is_object() || !face.contains("bbox") || !face.at("bbox").is_array() ||
          face.at("bbox").size() != 4) {
        continue;
      }
      const auto& bbox = face.at("bbox");
      if (std::all_of(bbox.begin(), bbox.end(),
                      [](const auto& value) { return value.is_number(); }) &&
          (bbox.at(2).get<double>() < bbox.at(0).get<double>() ||
           bbox.at(3).get<double>() < bbox.at(1).get<double>())) {
        return "faces[" + std::to_string(index) + "] has an inverted bbox";
      }
    }
    return {};
  }

  const char* items = nullptr;
  const char* count = nullptr;
  if (capability == Capability::kDetection) {
    items = "objects";
    count = "object_count";
  } else if (capability == Capability::kHuman || capability == Capability::kHumanPose ||
             capability == Capability::kPose || capability == Capability::kPosture) {
    items = "persons";
    count = "person_count";
  } else {
    return {};
  }
  if (!response.is_object() || !response.contains(items) || !response.at(items).is_array() ||
      !response.contains(count) || !response.at(count).is_number_unsigned()) {
    return {};
  }
  if (response.at(items).size() != response.at(count).get<std::size_t>()) {
    return std::string(count) + " does not match " + items + " length";
  }
  for (std::size_t index = 0; index < response.at(items).size(); ++index) {
    const auto& item = response.at(items).at(index);
    if (!item.is_object() || !item.contains("bbox") || !item.at("bbox").is_array() ||
        item.at("bbox").size() != 4) {
      continue;
    }
    const auto& bbox = item.at("bbox");
    if (!std::all_of(bbox.begin(), bbox.end(),
                     [](const auto& value) { return value.is_number(); })) {
      continue;
    }
    if (bbox.at(2).get<double>() < bbox.at(0).get<double>() ||
        bbox.at(3).get<double>() < bbox.at(1).get<double>()) {
      return std::string(items) + "[" + std::to_string(index) + "] has an inverted bbox";
    }
  }
  return {};
}

}  // namespace

std::string_view CapabilityName(Capability capability) {
  switch (capability) {
    case Capability::kVad:
      return "audio.vad";
    case Capability::kAsr:
      return "audio.transcription";
    case Capability::kTts:
      return "audio.synthesis";
    case Capability::kDetection:
      return "vision.object_detection";
    case Capability::kHuman:
      return "vision.human";
    case Capability::kHumanPose:
      return "vision.human_pose";
    case Capability::kPose:
      return "vision.pose";
    case Capability::kPosture:
      return "vision.posture";
    case Capability::kPostureClassification:
      return "vision.posture_classification";
    case Capability::kVlm:
      return "vision.vlm";
    case Capability::kFace:
      return "vision.face";
    case Capability::kArmGraspPlan:
      return "arm.grasp_plan";
    case Capability::kChatCompletion:
      return "text.chat_completion";
  }
  return "unknown";
}

std::vector<ModelConfig> ParseConfig(std::string_view text, const std::string& path) {
  toml::table root;
  try {
    root = toml::parse(text, path);
  } catch (const toml::parse_error& error) {
    throw std::runtime_error("models '" + path + "': " + std::string(error.description()));
  }
  const toml::node* perception_node = root.get("perception");
  if (perception_node == nullptr) return {};
  const toml::table* perception = perception_node->as_table();
  if (perception == nullptr) Fail(path, "key 'perception' must be a table");
  const toml::node* models_node = perception->get("model");
  if (models_node == nullptr) return {};
  const toml::array* model_array = models_node->as_array();
  if (model_array == nullptr) Fail(path, "key 'model' must be an array of tables");

  std::vector<ModelConfig> models;
  std::vector<std::string> model_ids;
  std::size_t index = 0;
  for (const toml::node& node : *model_array) {
    const toml::table* table = node.as_table();
    if (table == nullptr) Fail(path, "model[" + std::to_string(index) + "] must be a table");
    for (const auto& [key, value] : *table) {
      (void)value;
      if (key != "id" && key != "capability" && key != "endpoint" && key != "timeout_ms" &&
          key != "health_endpoint" && key != "health_interval_ms" && key != "health_timeout_ms" &&
          key != "transport" && key != "wire_format" && key != "http_method" &&
          key != "api_key_env" && key != "provider_model_id" && key != "file_field" &&
          key != "adapter" && key != "max_request_bytes" && key != "max_response_bytes" &&
          key != "fields" && key != "enabled") {
        Fail(path, "unknown key '" + std::string(key.str()) + "' in model[" +
                       std::to_string(index) + "]");
      }
    }
    auto RequiredString = [&](const char* key) {
      const toml::node* value = table->get(key);
      if (value == nullptr || !value->is_string()) {
        Fail(path, "model[" + std::to_string(index) + "] key '" + key + "' must be a string");
      }
      const std::string parsed = value->value<std::string>().value();
      if (parsed.empty()) {
        Fail(path, "model[" + std::to_string(index) + "] key '" + key + "' must be non-empty");
      }
      return parsed;
    };

    ModelConfig model;
    model.id = RequiredString("id");
    if (!ValidName(model.id)) {
      Fail(path, "model[" + std::to_string(index) + "] id contains invalid characters");
    }
    if (const toml::node* enabled = table->get("enabled"); enabled != nullptr) {
      const auto value = enabled->value<bool>();
      if (!value) {
        Fail(path, "model[" + std::to_string(index) + "] key 'enabled' must be a boolean");
      }
      model.enabled = *value;
    }
    model.capability = ParseCapability(RequiredString("capability"), path, index);
    if (const toml::node* transport = table->get("transport"); transport != nullptr) {
      const auto value = transport->value<std::string>();
      if (!value || (*value != "http" && *value != "websocket" && *value != "grpc")) {
        Fail(path,
             "model[" + std::to_string(index) + "] key 'transport' must be 'http', 'websocket', or 'grpc'");
      }
      model.transport = *value == "websocket" ? Transport::kWebSocket
                      : *value == "grpc" ? Transport::kGrpc : Transport::kHttp;
    }
    if (const toml::node* method = table->get("http_method"); method != nullptr) {
      const auto value = method->value<std::string>();
      if (!value || value->empty()) {
        Fail(path, "model[" + std::to_string(index) + "] key 'http_method' must be a string");
      }
      model.http_method = ParseHttpMethod(*value, path, index);
    }
    if (const toml::node* wire = table->get("wire_format"); wire != nullptr) {
      const auto value = wire->value<std::string>();
      if (!value || (*value != "json" && *value != "multipart")) {
        Fail(path, "model[" + std::to_string(index) +
                       "] key 'wire_format' must be 'json' or 'multipart'");
      }
      model.wire_format = *value == "multipart" ? WireFormat::kMultipart : WireFormat::kJson;
    }
    if (model.transport == Transport::kWebSocket) {
      const bool supported =
          model.capability == Capability::kDetection || model.capability == Capability::kHuman ||
          model.capability == Capability::kHumanPose || model.capability == Capability::kPose ||
          model.capability == Capability::kPosture || model.capability == Capability::kFace ||
          model.capability == Capability::kArmGraspPlan;
      if (!supported) {
        Fail(path, "model[" + std::to_string(index) +
                       "] websocket transport requires a vision or arm.grasp_plan capability");
      }
    }
    model.endpoint = RequiredString("endpoint");
    try {
      ParseModelEndpoint(model, "models '" + path + "': model[" + std::to_string(index) + "]");
    } catch (const std::exception& error) {
      Fail(path, "model[" + std::to_string(index) + "] " + error.what());
    }
    if (model.transport == Transport::kWebSocket && model.wire_format != WireFormat::kJson) {
      Fail(path,
           "model[" + std::to_string(index) + "] websocket transport requires json wire_format");
    }
    if (model.transport == Transport::kGrpc && model.wire_format != WireFormat::kJson) {
      Fail(path, "model[" + std::to_string(index) + "] grpc transport requires json wire_format");
    }
    if (table->get("health_endpoint") != nullptr) {
      model.health_endpoint = RequiredString("health_endpoint");
      if (model.transport == Transport::kGrpc) {
        Fail(path, "model[" + std::to_string(index) + "] gRPC Health uses the model endpoint; omit health_endpoint");
      }
      ParseEndpoint(model.health_endpoint,
                    "models '" + path + "': model[" + std::to_string(index) + "] health");
    }

    if (const toml::node* timeout = table->get("timeout_ms"); timeout != nullptr) {
      const auto value = timeout->value<std::int64_t>();
      if (!value || *value <= 0 || *value > 300000) {
        Fail(path, "model[" + std::to_string(index) +
                       "] key 'timeout_ms' must be an integer in (0, 300000]");
      }
      model.timeout = std::chrono::milliseconds(*value);
    }
    const std::pair<const char*, std::chrono::milliseconds*> health_durations[] = {
        {"health_interval_ms", &model.health_interval},
        {"health_timeout_ms", &model.health_timeout}};
    for (const auto& [key, target] : health_durations) {
      if (const toml::node* duration = table->get(key); duration != nullptr) {
        const auto value = duration->value<std::int64_t>();
        const std::int64_t maximum =
            std::string_view(key) == "health_interval_ms" ? 3600000 : 300000;
        if (!value || *value <= 0 || *value > maximum) {
          Fail(path, "model[" + std::to_string(index) + "] key '" + key +
                         "' must be an integer in (0, " + std::to_string(maximum) + "]");
        }
        *target = std::chrono::milliseconds(*value);
      }
    }
    if (const toml::node* adapter = table->get("adapter"); adapter != nullptr) {
      const auto value = adapter->value<std::string>();
      if (!value || value->empty()) {
        Fail(path, "model[" + std::to_string(index) + "] key 'adapter' must be a non-empty string");
      }
      model.adapter = *value;
    }
    for (const char* key : {"api_key_env", "provider_model_id", "file_field"}) {
      if (const toml::node* node_value = table->get(key); node_value != nullptr) {
        const auto value = node_value->value<std::string>();
        if (!value || value->empty()) {
          Fail(path,
               "model[" + std::to_string(index) + "] key '" + key + "' must be a non-empty string");
        }
        if (std::string(key) == "api_key_env") {
          model.api_key_env = *value;
        } else if (std::string(key) == "provider_model_id") {
          model.provider_model_id = *value;
        } else {
          try {
            CheckMultipartToken(*value, "file_field");
          } catch (const std::exception& error) {
            Fail(path, "model[" + std::to_string(index) + "] " + error.what());
          }
          model.file_field = *value;
        }
      }
    }
    const std::pair<const char*, std::size_t*> byte_limits[] = {
        {"max_request_bytes", &model.max_request_bytes},
        {"max_response_bytes", &model.max_response_bytes}};
    for (const auto& [key, target] : byte_limits) {
      if (const toml::node* node_value = table->get(key); node_value != nullptr) {
        const auto value = node_value->value<std::int64_t>();
        if (!value || *value <= 0) {
          Fail(path,
               "model[" + std::to_string(index) + "] key '" + key + "' must be a positive integer");
        }
        *target = static_cast<std::size_t>(*value);
      }
    }
    if (const toml::node* fields = table->get("fields"); fields != nullptr) {
      const toml::table* field_table = fields->as_table();
      if (field_table == nullptr) {
        Fail(path, "model[" + std::to_string(index) + "] key 'fields' must be a table");
      }
      for (const auto& [key, value] : *field_table) {
        try {
          CheckMultipartToken(std::string(key.str()), "field name");
        } catch (const std::exception& error) {
          Fail(path, "model[" + std::to_string(index) + "] " + error.what());
        }
        const auto field_value = value.value<std::string>();
        if (!field_value) {
          Fail(path, "model[" + std::to_string(index) + "] field '" + std::string(key.str()) +
                         "' must be a string");
        }
        model.fields.emplace_back(std::string(key.str()), *field_value);
      }
    }
    if (model.transport == Transport::kWebSocket) {
      for (const char* key : {"http_method", "api_key_env", "file_field", "fields"}) {
        if (table->get(key) != nullptr) {
          Fail(path, "model[" + std::to_string(index) + "] key '" + key +
                         "' is not supported by websocket transport");
        }
      }
    }
    if (model.transport == Transport::kGrpc) {
      for (const char* key : {"http_method", "api_key_env", "file_field", "fields"}) {
        if (table->get(key) != nullptr) {
          Fail(path, "model[" + std::to_string(index) + "] key '" + key +
                         "' is not supported by grpc transport");
        }
      }
      if (model.adapter != "canonical") {
        Fail(path, "model[" + std::to_string(index) +
                       "] grpc transport requires the canonical adapter");
      }
    }

    const std::string stem = ContractStem(model.capability);
    model.request_contract = "perception/model/" + stem + "_request.v1.json";
    model.response_contract = "perception/model/" + stem + "_response.v1.json";
    if (std::find(model_ids.begin(), model_ids.end(), model.id) != model_ids.end()) {
      Fail(path, "duplicate model id '" + model.id + "'");
    }
    model_ids.push_back(model.id);
    models.push_back(std::move(model));
    ++index;
  }
  std::sort(models.begin(), models.end(),
            [](const ModelConfig& left, const ModelConfig& right) { return left.id < right.id; });
  return models;
}

struct Runtime::Impl {
  struct Model {
    ModelConfig config;
    Endpoint endpoint;
    Endpoint health_endpoint;
    std::shared_ptr<const Adapter> adapter;
    std::unique_ptr<std::timed_mutex> stream_mutex;
    std::unique_ptr<StreamSession> stream;
  };

  std::vector<Model> models;
  ContractValidator validate;
  Runtime* owner{nullptr};
  struct Job {
    std::string id;
    nlohmann::json request;
    Completion complete;
    CancelRequested cancel_requested;
    std::string correlation_id;
    std::chrono::steady_clock::time_point enqueued_at;
  };
  std::mutex mutex;
  std::condition_variable wakeup;
  std::condition_variable health_wakeup;
  std::deque<Job> queue;
  std::vector<std::thread> workers;
  std::vector<std::thread> health_workers;
  struct HealthSchedule {
    std::size_t model_index;
    std::chrono::steady_clock::time_point due;
    bool active{false};
  };
  std::vector<HealthSchedule> health_schedule;
  std::map<std::string, ModelHealth> health;
  bool stopping{false};

  void Run(Job job) {
    std::optional<eos::lib::CorrelationScope> correlation;
    if (!job.correlation_id.empty()) correlation.emplace(job.correlation_id);

    eos::lib::LogEvent(ModelsLogger(), spdlog::level::debug, "model_worker_call_started",
                       {{"model_id", job.id},
                        {"queue_wait_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now() - job.enqueued_at)
                                              .count()}});
    CallResult result;
    try {
      const CancelRequested cancel_requested = [this, &job] {
        bool runtime_stopping = false;
        {
          std::lock_guard<std::mutex> lock(mutex);
          runtime_stopping = stopping;
        }
        return runtime_stopping || (job.cancel_requested && job.cancel_requested());
      };
      result = owner->CallCancellable(job.id, job.request, cancel_requested);
    } catch (const std::exception& error) {
      eos::lib::LogEvent(ModelsLogger(), spdlog::level::err, "model_worker_exception",
                         {{"model_id", job.id}, {"exception", "standard"}});
      result.status = CallResult::Status::kInternal;
      result.message = "model call failed internally: " + std::string(error.what());
    } catch (...) {
      eos::lib::LogEvent(ModelsLogger(), spdlog::level::err, "model_worker_exception",
                         {{"model_id", job.id}, {"exception", "unknown"}});
      result.status = CallResult::Status::kInternal;
      result.message = "model call failed internally";
    }
    try {
      job.complete(std::move(result));
    } catch (const std::exception&) {
      eos::lib::LogEvent(ModelsLogger(), spdlog::level::warn, "model_completion_exception_dropped",
                         {{"model_id", job.id}});
    } catch (...) {
      eos::lib::LogEvent(ModelsLogger(), spdlog::level::warn, "model_completion_exception_dropped",
                         {{"model_id", job.id}});
    }
  }

  void Start(Runtime* runtime) {
    owner = runtime;
    try {
    for (std::size_t i = 0; i < kModelWorkerCount; ++i) {
      workers.emplace_back([this, i] {
        eos::lib::LogEvent(ModelsLogger(), spdlog::level::info, "model_worker_started",
                           {{"worker_index", i}});
        while (true) {
          Job job;
          {
            std::unique_lock<std::mutex> lock(mutex);
            wakeup.wait(lock, [this] { return stopping || !queue.empty(); });
            if (stopping && queue.empty()) break;
            job = std::move(queue.front());
            queue.pop_front();
          }
          Run(std::move(job));
        }
        eos::lib::LogEvent(ModelsLogger(), spdlog::level::info, "model_worker_stopped",
                           {{"worker_index", i}});
      });
    }
    eos::lib::LogEvent(
        ModelsLogger(), spdlog::level::info, "model_worker_pool_started",
        {{"worker_count", kModelWorkerCount}, {"queue_capacity", kMaxQueuedModelCalls}});
    const auto health_start = std::chrono::steady_clock::now();
    for (std::size_t model_index = 0; model_index < models.size(); ++model_index) {
      const Model& model = models[model_index];
      if (model.config.enabled &&
          (!model.config.health_endpoint.empty() || model.config.transport == Transport::kGrpc))
        health_schedule.push_back({model_index, health_start});
    }
    const std::size_t health_worker_count =
        std::min(kHealthWorkerCount, health_schedule.size());
    for (std::size_t worker_index = 0; worker_index < health_worker_count; ++worker_index) {
      health_workers.emplace_back([this, worker_index] {
        try {
        eos::lib::LogEvent(ModelsLogger(), spdlog::level::info, "model_health_worker_started",
                            {{"worker_index", worker_index}});
        while (true) {
          std::size_t schedule_index = 0;
          std::size_t model_index = 0;
          {
            std::unique_lock<std::mutex> lock(mutex);
            while (true) {
              if (stopping) break;
              const auto now = std::chrono::steady_clock::now();
              auto selected = health_schedule.end();
              for (auto it = health_schedule.begin(); it != health_schedule.end(); ++it) {
                if (!it->active &&
                    (selected == health_schedule.end() || it->due < selected->due))
                  selected = it;
              }
              if (selected == health_schedule.end()) {
                health_wakeup.wait(lock);
                continue;
              }
              if (selected->due > now) {
                health_wakeup.wait_until(lock, selected->due);
                continue;
              }
              schedule_index = static_cast<std::size_t>(selected - health_schedule.begin());
              selected->active = true;
              model_index = selected->model_index;
              break;
            }
            if (stopping) break;
          }
          const Model& model = models[model_index];
          try {
            const CancelRequested cancelled = [this] {
              std::lock_guard<std::mutex> lock(mutex);
              return stopping;
            };
            ModelHealth probed =
                ProbeHealth(model.config, model.health_endpoint, *model.adapter, cancelled);
            HealthStatus previous_status = HealthStatus::kChecking;
            {
              std::lock_guard<std::mutex> lock(mutex);
              previous_status = health[model.config.id].status;
              health[model.config.id] = probed;
              health_schedule[schedule_index].active = false;
              health_schedule[schedule_index].due =
                  std::chrono::steady_clock::now() + model.config.health_interval;
            }
            health_wakeup.notify_all();
            const bool became_unhealthy =
                probed.status != HealthStatus::kReady && probed.status != previous_status &&
                (previous_status == HealthStatus::kReady ||
                 previous_status == HealthStatus::kDegraded);
            const bool warn = became_unhealthy ||
                              (probed.status == HealthStatus::kDegraded &&
                               previous_status == HealthStatus::kChecking);
            eos::lib::LogEvent(
                ModelsLogger(), warn ? spdlog::level::warn : spdlog::level::debug,
                "model_health_checked",
                {{"model_id", model.config.id},
                 {"status", HealthStatusName(probed.status)},
                 {"health_timeout_ms", model.config.health_timeout.count()},
                 {"message", probed.message}},
                warn ? "Model health check failed" : "");
          } catch (...) {
            {
              std::lock_guard<std::mutex> lock(mutex);
              auto& failed = health[model.config.id];
              failed.model_id = model.config.id;
              failed.status = HealthStatus::kUnavailable;
              failed.message = "health probe failed";
              failed.checked_at = std::chrono::system_clock::now();
              health_schedule[schedule_index].active = false;
              health_schedule[schedule_index].due =
                  std::chrono::steady_clock::now() + model.config.health_interval;
            }
            health_wakeup.notify_all();
            try {
              eos::lib::LogEvent(ModelsLogger(), spdlog::level::err,
                                 "model_health_worker_exception",
                                 {{"model_id", model.config.id}});
            } catch (...) {
            }
          }
        }
        eos::lib::LogEvent(ModelsLogger(), spdlog::level::info, "model_health_worker_stopped",
                            {{"worker_index", worker_index}});
        } catch (...) {
          try {
            eos::lib::LogEvent(ModelsLogger(), spdlog::level::err,
                               "model_health_worker_exception",
                               {{"worker_index", worker_index}});
          } catch (...) {
          }
        }
      });
    }
    eos::lib::LogEvent(ModelsLogger(), spdlog::level::info,
                       "model_health_worker_pool_started",
                       {{"worker_count", health_worker_count},
                        {"model_count", health_schedule.size()}});
    } catch (...) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        stopping = true;
      }
      wakeup.notify_all();
      health_wakeup.notify_all();
      for (std::thread& health_worker : health_workers)
        if (health_worker.joinable()) health_worker.join();
      for (std::thread& worker : workers)
        if (worker.joinable()) worker.join();
      throw;
    }
  }

  void Stop() {
    std::deque<Job> pending;
    {
      std::lock_guard<std::mutex> lock(mutex);
      stopping = true;
      pending = std::move(queue);
    }
    eos::lib::LogEvent(ModelsLogger(), spdlog::level::info, "model_shutdown_started",
                       {{"pending_calls", pending.size()}});
    wakeup.notify_all();
    health_wakeup.notify_all();
    for (std::thread& health_worker : health_workers) health_worker.join();
    for (Job& job : pending) {
      std::optional<eos::lib::CorrelationScope> correlation;
      if (!job.correlation_id.empty()) correlation.emplace(job.correlation_id);
      CallResult result;
      result.status = CallResult::Status::kUnavailable;
      result.message = "model worker pool is stopping";
      try {
        job.complete(std::move(result));
      } catch (const std::exception&) {
        eos::lib::LogEvent(ModelsLogger(), spdlog::level::warn,
                           "model_completion_exception_dropped", {{"model_id", job.id}});
      } catch (...) {
        eos::lib::LogEvent(ModelsLogger(), spdlog::level::warn,
                           "model_completion_exception_dropped", {{"model_id", job.id}});
      }
    }
    for (std::thread& worker : workers) worker.join();
    eos::lib::LogEvent(ModelsLogger(), spdlog::level::info, "model_shutdown_completed", {});
  }
};

Runtime::Runtime() : impl_(std::make_unique<Impl>()) { impl_->Start(this); }

Runtime::Runtime(std::vector<ModelConfig> models, ContractValidator validate,
                 AdapterFactory adapter_factory)
    : impl_(std::make_unique<Impl>()) {
  const bool has_enabled = std::any_of(models.begin(), models.end(),
                                       [](const ModelConfig& model) { return model.enabled; });
  if (has_enabled && !validate) {
    auto schemas = std::make_shared<ModelSchemaStore>();
    validate = [schemas = std::move(schemas)](const std::string& contract,
                                              const nlohmann::json& instance) {
      schemas->Validate(contract, instance);
    };
  }
  impl_->validate = std::move(validate);
  std::sort(models.begin(), models.end(),
            [](const ModelConfig& left, const ModelConfig& right) { return left.id < right.id; });
  for (std::size_t index = 1; index < models.size(); ++index) {
    if (models[index - 1].id == models[index].id) {
      eos::lib::LogEvent(ModelsLogger(), spdlog::level::warn, "model_admission_rejected",
                         {{"model_id", models[index].id}, {"reason", "duplicate_model_id"}});
      throw std::runtime_error("duplicate model id '" + models[index].id + "'");
    }
  }
  for (ModelConfig& model : models) {
    Impl::Model admitted;
    try {
      admitted.endpoint = ParseModelEndpoint(model, "model '" + model.id + "'");
      if (model.transport == Transport::kGrpc && model.adapter != "canonical") {
        throw std::runtime_error("model '" + model.id +
                                 "' gRPC transport requires the canonical adapter");
      }
      if (!model.health_endpoint.empty()) {
        admitted.health_endpoint =
            ParseEndpoint(model.health_endpoint, "model '" + model.id + "' health");
      }
      if (!adapter_factory && model.adapter != "canonical") {
        throw std::runtime_error("model '" + model.id + "' adapter '" + model.adapter +
                                 "' requires an adapter factory");
      }
      admitted.adapter =
          adapter_factory ? adapter_factory(model) : std::make_shared<CanonicalAdapter>();
      if (!admitted.adapter) throw std::runtime_error("adapter factory returned null");
    } catch (const std::exception& error) {
      eos::lib::LogEvent(
          ModelsLogger(), spdlog::level::warn, "model_admission_rejected",
          {{"model_id", model.id}, {"reason", "invalid_configuration"}, {"error", error.what()}});
      throw;
    }
    admitted.config = std::move(model);
    if (admitted.config.enabled && admitted.config.transport == Transport::kWebSocket) {
      admitted.stream_mutex = std::make_unique<std::timed_mutex>();
      admitted.stream = admitted.adapter->CreateStreamSession(admitted.config);
      if (!admitted.stream) {
        throw std::runtime_error("model '" + admitted.config.id + "' adapter '" +
                                 admitted.config.adapter +
                                 "' does not implement its WebSocket protocol");
      }
    }
    if (admitted.config.enabled) {
      if (admitted.config.health_endpoint.empty() && admitted.config.transport != Transport::kGrpc) {
        eos::lib::LogOnce(ModelsLogger(), spdlog::level::warn,
                          "model-health-missing:" + admitted.config.id,
                          "model_health_not_configured", {{"model_id", admitted.config.id}});
      }
      ModelHealth initial;
      initial.model_id = admitted.config.id;
      if (admitted.config.health_endpoint.empty() && admitted.config.transport != Transport::kGrpc) {
        initial.status = HealthStatus::kUnavailable;
        initial.message = "health endpoint is not configured";
      } else {
        initial.status = HealthStatus::kChecking;
      }
      impl_->health.emplace(admitted.config.id, std::move(initial));
    }
    if (admitted.config.enabled) {
      nlohmann::json fields{
          {"model_id", admitted.config.id},
          {"capability", std::string(CapabilityName(admitted.config.capability))},
          {"provider", admitted.config.transport == Transport::kWebSocket ? "websocket" :
                       admitted.config.transport == Transport::kGrpc ? "grpc" : "http"},
          {"adapter", admitted.config.adapter},
          {"wire_format", WireFormatName(admitted.config.wire_format)},
          {"timeout_ms", admitted.config.timeout.count()},
          {"max_request_bytes", admitted.config.max_request_bytes},
          {"max_response_bytes", admitted.config.max_response_bytes}};
      fields["endpoint"] = admitted.config.transport == Transport::kGrpc
                               ? admitted.config.endpoint : admitted.endpoint.authority;
      eos::lib::LogEvent(ModelsLogger(), spdlog::level::info, "model_admitted", fields);
    } else {
      eos::lib::LogEvent(ModelsLogger(), spdlog::level::info, "model_disabled",
                         {{"model_id", admitted.config.id},
                          {"capability", std::string(CapabilityName(admitted.config.capability))}});
    }
    impl_->models.push_back(std::move(admitted));
  }
  impl_->Start(this);
}

Runtime::~Runtime() {
  if (impl_) impl_->Stop();
}
Runtime::Runtime(Runtime&& other) noexcept : impl_(std::move(other.impl_)) {
  if (impl_) impl_->owner = this;
}
Runtime& Runtime::operator=(Runtime&& other) noexcept {
  if (this == &other) return *this;
  if (impl_) impl_->Stop();
  impl_ = std::move(other.impl_);
  if (impl_) impl_->owner = this;
  return *this;
}

std::vector<ModelConfig> Runtime::Discover() const {
  std::vector<ModelConfig> models;
  models.reserve(impl_->models.size());
  for (const Impl::Model& model : impl_->models) {
    if (model.config.enabled) models.push_back(model.config);
  }
  return models;
}

std::vector<ModelConfig> Runtime::ConfiguredAll() const {
  std::vector<ModelConfig> models;
  models.reserve(impl_->models.size());
  for (const Impl::Model& model : impl_->models) models.push_back(model.config);
  return models;
}

const ModelConfig* Runtime::Find(const std::string& id) const {
  const auto found = std::lower_bound(
      impl_->models.begin(), impl_->models.end(), id,
      [](const Impl::Model& model, const std::string& value) { return model.config.id < value; });
  return found != impl_->models.end() && found->config.id == id && found->config.enabled
             ? &found->config
             : nullptr;
}

ModelReferenceState Runtime::Resolve(const std::string& id, Capability expected) const {
  const auto found = std::lower_bound(
      impl_->models.begin(), impl_->models.end(), id,
      [](const Impl::Model& model, const std::string& value) { return model.config.id < value; });
  if (found == impl_->models.end() || found->config.id != id) {
    return ModelReferenceState::kMissing;
  }
  const bool unified_posture = found->config.capability == Capability::kVlm &&
                               found->config.adapter == "gemma4-unified-v1" &&
                               expected == Capability::kPostureClassification;
  if (found->config.capability != expected && !unified_posture)
    return ModelReferenceState::kWrongCapability;
  return found->config.enabled ? ModelReferenceState::kEnabled : ModelReferenceState::kDisabled;
}

std::optional<ModelHealth> Runtime::Health(const std::string& id) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto found = impl_->health.find(id);
  if (found == impl_->health.end()) return std::nullopt;
  return found->second;
}

std::optional<ModelConfig> Runtime::Configured(const std::string& id) const {
  const auto found = std::lower_bound(
      impl_->models.begin(), impl_->models.end(), id,
      [](const Impl::Model& model, const std::string& value) { return model.config.id < value; });
  if (found == impl_->models.end() || found->config.id != id) return std::nullopt;
  return found->config;
}

std::vector<ModelHealth> Runtime::HealthAll() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::vector<ModelHealth> result;
  result.reserve(impl_->health.size());
  for (const auto& [id, health] : impl_->health) {
    (void)id;
    result.push_back(health);
  }
  return result;
}

CallResult Runtime::Call(const std::string& id, const nlohmann::json& request) const {
  return CallCancellable(id, request, {});
}

CallResult Runtime::Call(const std::string& id, const nlohmann::json& request,
                         const CancelRequested& cancel_requested) const {
  return CallCancellable(id, request, cancel_requested);
}

CallResult Runtime::RegisterFace(const std::string& id, const nlohmann::json& input,
                                 const std::string& name) const {
  const auto found = std::lower_bound(
      impl_->models.begin(), impl_->models.end(), id,
      [](const Impl::Model& model, const std::string& value) { return model.config.id < value; });
  if (found == impl_->models.end() || found->config.id != id || !found->config.enabled) {
    return {CallResult::Status::kNotFound, 0, {}, "unknown model '" + id + "'"};
  }
  if (found->config.capability != Capability::kFace ||
      found->config.transport != Transport::kWebSocket) {
    return {CallResult::Status::kInvalidArgument,
            0,
            {},
            "model '" + id + "' is not a WebSocket face model"};
  }
  if (!input.is_object() || !input.contains("data_b64") || !input.at("data_b64").is_string() ||
      input.at("data_b64").get_ref<const std::string&>().empty() || name.empty() ||
      name.size() > 256) {
    return {CallResult::Status::kInvalidArgument, 0, {}, "invalid internal face registration"};
  }
  const nlohmann::json request{{"operation", "register"}, {"input", input}, {"name", name}};
  if (request.dump().size() > found->config.max_request_bytes) {
    return {CallResult::Status::kInvalidArgument,
            0,
            {},
            "model '" + id + "' request exceeds configured byte limit"};
  }
  std::unique_lock<std::timed_mutex> lock(*found->stream_mutex, std::defer_lock);
  if (!lock.try_lock_for(found->config.timeout)) {
    return {CallResult::Status::kDeadlineExceeded,
            0,
            {},
            "model '" + id + "' registration exceeded " +
                std::to_string(found->config.timeout.count()) + " ms"};
  }
  CallResult result = found->stream->Call(request, {});
  if (result.status != CallResult::Status::kCompleted) return result;
  if (!result.response.is_object() || result.response.value("operation", "") != "register" ||
      !result.response.contains("data") || !result.response.at("data").is_object() ||
      !result.response.at("data").contains("name") ||
      !result.response.at("data").at("name").is_string() ||
      result.response.at("data").at("name").get_ref<const std::string&>().empty()) {
    return {CallResult::Status::kProtocolError,
            result.http_status,
            {},
            "model '" + id + "' returned an invalid registration response"};
  }
  return result;
}

CallResult Runtime::CallCancellable(const std::string& id, const nlohmann::json& request,
                                    const CancelRequested& cancel_requested) const {
  const auto started = std::chrono::steady_clock::now();
  const std::size_t request_bytes = request.dump().size();
  bool specific_protocol_failure = false;
  const auto found = std::lower_bound(
      impl_->models.begin(), impl_->models.end(), id,
      [](const Impl::Model& model, const std::string& value) { return model.config.id < value; });
  if (found == impl_->models.end() || found->config.id != id || !found->config.enabled) {
    eos::lib::LogEvent(ModelsLogger(), spdlog::level::warn, "model_call_completed",
                       {{"model_id", id},
                        {"status", "not_found"},
                        {"request_bytes", request_bytes},
                        {"duration_ms", 0}});
    return {CallResult::Status::kNotFound, 0, {}, "unknown model '" + id + "'"};
  }
  if (request_bytes > found->config.max_request_bytes) {
    return {CallResult::Status::kInvalidArgument,
            0,
            {},
            "model '" + id + "' request exceeds configured byte limit"};
  }
  eos::lib::LogEvent(ModelsLogger(), spdlog::level::debug, "model_call_started",
                     {{"model_id", id},
                      {"capability", std::string(CapabilityName(found->config.capability))},
                      {"provider", found->config.transport == Transport::kHttp ? "http"
                                   : found->config.transport == Transport::kGrpc ? "grpc" : "websocket"},
                      {"adapter", found->config.adapter},
                      {"request_bytes", request_bytes}});
  const nlohmann::json& resolved_request = request;
  const bool unified_posture_request = found->config.adapter == "gemma4-unified-v1" &&
                                       request.is_object() && request.contains("bbox");
  const std::string request_contract = unified_posture_request
                                           ? "perception/model/gemma_posture_request.v1.json"
                                           : found->config.request_contract;
  const std::string response_contract = unified_posture_request
                                            ? "perception/model/gemma_posture_response.v1.json"
                                            : found->config.response_contract;
  if (impl_->validate) {
    try {
      impl_->validate(request_contract, resolved_request);
    } catch (const std::exception& error) {
      eos::lib::LogEvent(ModelsLogger(), spdlog::level::warn, "model_call_completed",
                         {{"model_id", id},
                          {"status", "invalid_argument"},
                          {"stage", "request_validation"},
                          {"request_bytes", request_bytes},
                          {"duration_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now() - started)
                                              .count()}});
      return {CallResult::Status::kInvalidArgument, 0, {}, error.what()};
    }
    eos::lib::LogEvent(ModelsLogger(), spdlog::level::trace, "model_request_validated",
                       {{"model_id", id}, {"contract", request_contract}});
  }
  const bool stream_requested = request.is_object() && request.value("stream", false);
  if (stream_requested) {
    eos::lib::LogEvent(ModelsLogger(), spdlog::level::warn, "model_call_completed",
                       {{"model_id", id},
                        {"status", "invalid_argument"},
                        {"stage", "request_options"},
                        {"request_bytes", request_bytes},
                        {"duration_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now() - started)
                                            .count()}});
    return {CallResult::Status::kInvalidArgument,
            0,
            {},
            "model '" + id + "' uses the buffered call API, which does not support streaming"};
  }
  CallResult result;
  if (found->config.transport == Transport::kWebSocket) {
    if (cancel_requested && cancel_requested()) {
      result = {
          CallResult::Status::kUnavailable, 0, {}, "model '" + id + "' request was cancelled"};
    } else {
      const auto stream_deadline = started + found->config.timeout;
      std::unique_lock<std::timed_mutex> stream_lock(*found->stream_mutex, std::defer_lock);
      while (!stream_lock.try_lock_until(std::min(
          stream_deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(10)))) {
        if (cancel_requested && cancel_requested()) {
          eos::lib::LogEvent(ModelsLogger(), spdlog::level::warn, "model_call_completed",
                             {{"model_id", id},
                              {"status", "unavailable"},
                              {"stage", "stream_queue"},
                              {"request_bytes", request_bytes},
                              {"duration_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                                  std::chrono::steady_clock::now() - started)
                                                  .count()}});
          return {
              CallResult::Status::kUnavailable, 0, {}, "model '" + id + "' request was cancelled"};
        }
        if (std::chrono::steady_clock::now() >= stream_deadline) {
          eos::lib::LogEvent(ModelsLogger(), spdlog::level::warn, "model_call_completed",
                             {{"model_id", id},
                              {"status", "deadline_exceeded"},
                              {"stage", "stream_queue"},
                              {"request_bytes", request_bytes},
                              {"duration_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                                  std::chrono::steady_clock::now() - started)
                                                  .count()}});
          return {CallResult::Status::kDeadlineExceeded,
                  0,
                  {},
                  "model '" + id + "' request exceeded " +
                      std::to_string(found->config.timeout.count()) + " ms"};
        }
      }
      if (cancel_requested && cancel_requested()) {
        result = {
            CallResult::Status::kUnavailable, 0, {}, "model '" + id + "' request was cancelled"};
      } else {
        eos::lib::LogEvent(ModelsLogger(), spdlog::level::trace, "model_transport_started",
                           {{"model_id", id}, {"transport", "websocket"}});
        result = found->stream->Call(resolved_request, cancel_requested);
      }
    }
  } else if (found->config.transport == Transport::kGrpc) {
    result = GrpcCall(found->config, resolved_request, cancel_requested);
  } else {
    nlohmann::json provider_request;
    try {
      provider_request = found->adapter->PrepareRequest(found->config, resolved_request);
      eos::lib::LogEvent(ModelsLogger(), spdlog::level::trace,
                         "model_adapter_request_prepared",
                         {{"model_id", id},
                          {"adapter", found->config.adapter},
                          {"provider_request_bytes", provider_request.dump().size()}});
    } catch (const std::exception& error) {
      eos::lib::LogEvent(ModelsLogger(), spdlog::level::warn, "model_call_completed",
                         {{"model_id", id},
                          {"status", "invalid_argument"},
                          {"stage", "adapter_request"},
                          {"request_bytes", request_bytes},
                          {"duration_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now() - started)
                                              .count()}});
      return {CallResult::Status::kInvalidArgument,
              0,
              {},
              "model '" + id + "' adapter rejected request: " + error.what()};
    }
    result =
        Post(found->config, found->endpoint, provider_request, *found->adapter, cancel_requested);
    eos::lib::LogEvent(ModelsLogger(), spdlog::level::trace, "model_transport_completed",
                       {{"model_id", id},
                        {"transport", "http"},
                        {"status", CallStatusName(result.status)},
                        {"http_status", result.http_status}});
    if (result.status == CallResult::Status::kCompleted) {
      try {
        found->adapter->NormalizeResponseForRequest(found->config, resolved_request,
                                                    &result.response);
        eos::lib::LogEvent(ModelsLogger(), spdlog::level::trace,
                           "model_adapter_response_normalized",
                           {{"model_id", id}, {"adapter", found->config.adapter}});
      } catch (const std::exception& error) {
        result.status = CallResult::Status::kProtocolError;
        result.message = "model '" + id + "' adapter rejected response: " + error.what();
        specific_protocol_failure = true;
      }
    }
  }
  if (result.status == CallResult::Status::kCompleted && impl_->validate) {
    try {
      impl_->validate(response_contract, result.response);
    } catch (const std::exception& error) {
      result.status = CallResult::Status::kProtocolError;
      result.message = error.what();
      specific_protocol_failure = true;
    }
    if (result.status == CallResult::Status::kCompleted) {
      eos::lib::LogEvent(ModelsLogger(), spdlog::level::trace, "model_response_validated",
                         {{"model_id", id}, {"contract", response_contract}});
    }
  }
  if (result.status == CallResult::Status::kCompleted) {
    const std::string invariant_error =
        ResponseInvariantError(found->config.capability, request, result.response);
    if (!invariant_error.empty()) {
      result.status = CallResult::Status::kProtocolError;
      result.message =
          "model '" + found->config.id + "' returned inconsistent data: " + invariant_error;
      specific_protocol_failure = true;
    }
  }
  nlohmann::json fields{
      {"model_id", id},
      {"provider", found->config.transport == Transport::kHttp ? "http"
                   : found->config.transport == Transport::kGrpc ? "grpc" : "websocket"},
      {"adapter", found->config.adapter},
      {"status", CallStatusName(result.status)},
      {"http_status", result.http_status},
      {"request_bytes", request_bytes},
      {"response_bytes", result.response.is_null() ? 0 : result.response.dump().size()},
      {"duration_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - started)
                          .count()}};
  if (specific_protocol_failure) fields["stage"] = "response_validation";
  const auto level = result.status == CallResult::Status::kCompleted   ? spdlog::level::debug
                     : result.status == CallResult::Status::kInternal  ? spdlog::level::err
                     : result.status == CallResult::Status::kUnavailable
                         ? spdlog::level::debug
                         : spdlog::level::warn;
  eos::lib::LogEvent(ModelsLogger(), level, "model_call_completed", fields);
  return result;
}

bool Runtime::CallAsync(std::string id, nlohmann::json request, Completion complete,
                        CancelRequested cancel_requested) {
  const std::string correlation_id = eos::lib::CurrentCorrelationId();
  bool stopping = false;
  bool saturated = false;
  std::size_t queue_depth = 0;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    stopping = impl_->stopping;
    saturated = impl_->queue.size() >= kMaxQueuedModelCalls;
    if (!stopping && !saturated) {
      impl_->queue.push_back({std::move(id), std::move(request), std::move(complete),
                              std::move(cancel_requested), correlation_id,
                              std::chrono::steady_clock::now()});
      queue_depth = impl_->queue.size();
    }
  }
  if (stopping) {
    eos::lib::LogEvent(ModelsLogger(), spdlog::level::warn, "model_enqueue_rejected",
                       {{"reason", "stopping"}});
    return false;
  }
  if (saturated) {
    eos::lib::LogEvent(ModelsLogger(), spdlog::level::warn, "model_enqueue_rejected",
                       {{"reason", "queue_saturated"}, {"queue_capacity", kMaxQueuedModelCalls}});
    return false;
  }
  impl_->wakeup.notify_one();
  eos::lib::LogEvent(ModelsLogger(), spdlog::level::debug, "model_call_enqueued",
                     {{"queue_depth", queue_depth}, {"queue_capacity", kMaxQueuedModelCalls}});
  return true;
}

}  // namespace eos::runtime::models

/// Standalone HTTP/ProtoJSON and SSE front end for EOS gRPC.
#include "src/eos/ext/north_http/server/server.h"

#include <google/protobuf/empty.pb.h>
#include <google/protobuf/message.h>
#include <google/protobuf/util/json_util.h>
#include <grpcpp/generic/generic_stub.h>
#include <grpcpp/grpcpp.h>
#include <poll.h>
#include <sys/socket.h>

#include <algorithm>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "eos/gateway.grpc.pb.h"
#include "eos/skills.grpc.pb.h"
#include "eos/workflow.grpc.pb.h"
#include "src/eos/ext/north_http/audit/audit.h"
#include "src/eos/ext/north_http/client/client.h"
#include "src/eos/lib/log.h"

namespace eos::ext::north_http {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using json = nlohmann::json;

std::shared_ptr<spdlog::logger>& Logger() {
  static auto logger = eos::lib::GetLogger("north-http");
  return logger;
}

struct Target {
  std::string path;
  std::map<std::string, std::string> query;
  std::vector<std::string> segments;
};

int Hex(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  value = static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
  return value >= 'a' && value <= 'f' ? value - 'a' + 10 : -1;
}

std::string UrlDecode(const std::string& value) {
  std::string result;
  result.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '+') {
      result.push_back(' ');
    } else if (value[i] == '%' && i + 2 < value.size()) {
      const int high = Hex(value[i + 1]);
      const int low = Hex(value[i + 2]);
      if (high >= 0 && low >= 0) {
        result.push_back(static_cast<char>((high << 4) | low));
        i += 2;
      } else {
        result.push_back(value[i]);
      }
    } else {
      result.push_back(value[i]);
    }
  }
  return result;
}

Target ParseTarget(const std::string& raw) {
  Target result;
  const std::size_t question = raw.find('?');
  result.path = raw.substr(0, question);
  std::size_t start = 0;
  while (start < result.path.size()) {
    while (start < result.path.size() && result.path[start] == '/') ++start;
    if (start == result.path.size()) break;
    const std::size_t end = result.path.find('/', start);
    result.segments.push_back(
        UrlDecode(result.path.substr(start, end == std::string::npos ? end : end - start)));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  if (question == std::string::npos) return result;
  start = question + 1;
  while (start <= raw.size()) {
    const std::size_t end = raw.find('&', start);
    const std::string item = raw.substr(start, end == std::string::npos ? end : end - start);
    const std::size_t equal = item.find('=');
    const std::string key = UrlDecode(item.substr(0, equal));
    const std::string value = equal == std::string::npos ? "" : UrlDecode(item.substr(equal + 1));
    if (!key.empty()) result.query[key] = value;
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return result;
}

http::status HttpStatus(grpc::StatusCode code) {
  switch (code) {
    case grpc::StatusCode::CANCELLED:
      return static_cast<http::status>(499);
    case grpc::StatusCode::INVALID_ARGUMENT:
      return http::status::bad_request;
    case grpc::StatusCode::UNAUTHENTICATED:
      return http::status::unauthorized;
    case grpc::StatusCode::PERMISSION_DENIED:
      return http::status::forbidden;
    case grpc::StatusCode::NOT_FOUND:
      return http::status::not_found;
    case grpc::StatusCode::ALREADY_EXISTS:
    case grpc::StatusCode::ABORTED:
      return http::status::conflict;
    case grpc::StatusCode::FAILED_PRECONDITION:
      return http::status::precondition_failed;
    case grpc::StatusCode::RESOURCE_EXHAUSTED:
      return http::status::too_many_requests;
    case grpc::StatusCode::UNIMPLEMENTED:
      return http::status::not_implemented;
    case grpc::StatusCode::UNAVAILABLE:
      return http::status::service_unavailable;
    case grpc::StatusCode::DEADLINE_EXCEEDED:
      return http::status::gateway_timeout;
    case grpc::StatusCode::DATA_LOSS:
      return http::status::bad_gateway;
    default:
      return http::status::internal_server_error;
  }
}

std::string ErrorBody(grpc::StatusCode code, const std::string& message,
                      const std::string& request_id) {
  return json{{"code", static_cast<int>(code)}, {"message", message}, {"requestId", request_id}}
      .dump();
}

std::string MessageJson(const google::protobuf::Message& message) {
  std::string body;
  google::protobuf::util::JsonPrintOptions options;
  options.preserve_proto_field_names = false;
  const auto status = google::protobuf::util::MessageToJsonString(message, &body, options);
  if (!status.ok()) throw std::runtime_error("cannot encode ProtoJSON: " + status.ToString());
  return body;
}

template <typename Message>
bool ParseJson(const std::string& body, Message* message, std::string* error) {
  google::protobuf::util::JsonParseOptions options;
  options.ignore_unknown_fields = false;
  const auto status =
      google::protobuf::util::JsonStringToMessage(body.empty() ? "{}" : body, message, options);
  if (status.ok()) return true;
  *error = "malformed ProtoJSON request: " + status.ToString();
  return false;
}

std::uint64_t ParseSequence(const std::string& text, const char* name, std::string* error) {
  if (text.empty()) return 0;
  std::uint64_t result = 0;
  for (char c : text) {
    if (c < '0' || c > '9' || result > (UINT64_MAX - static_cast<unsigned int>(c - '0')) / 10) {
      *error = std::string(name) + " must be an unsigned integer";
      return 0;
    }
    result = result * 10 + static_cast<unsigned int>(c - '0');
  }
  return result;
}

bool ParseActionListCursor(const std::string& text, island::eos::v1::ActionListCursor* cursor,
                           std::string* error) {
  const std::size_t separator = text.rfind(':');
  if (separator == std::string::npos || separator == 0 || separator + 1 == text.size()) {
    *error = "cursor must have the form epoch:sequence";
    return false;
  }
  cursor->set_epoch(text.substr(0, separator));
  cursor->set_sequence(ParseSequence(text.substr(separator + 1), "cursor sequence", error));
  return error->empty();
}

struct Reply {
  http::status status{http::status::ok};
  std::string content_type{"application/json"};
  std::string body;
  bool no_store{false};
  std::string sample_age_ms;
};

struct RequestIdResolution {
  std::string value;
  std::string error;
};

RequestIdResolution ResolveHttpRequestId(const http::fields& headers) {
  std::size_t count = 0;
  for (const auto& header : headers)
    if (beast::iequals(header.name_string(), "X-Request-ID")) ++count;
  if (count > 1) return {eos::lib::ResolveRequestId(), "X-Request-ID must occur at most once"};
  const auto header = headers.find("X-Request-ID");
  const std::string supplied = header == headers.end() ? "" : std::string(header->value());
  try {
    return {eos::lib::ResolveRequestId(supplied), {}};
  } catch (const std::invalid_argument&) {
    return {eos::lib::ResolveRequestId(), "X-Request-ID is invalid"};
  }
}

Reply BadRequest(const std::string& message, const std::string& request_id) {
  return {http::status::bad_request, "application/json",
          ErrorBody(grpc::StatusCode::INVALID_ARGUMENT, message, request_id)};
}

Reply NotFound(const std::string& request_id) {
  return {http::status::not_found, "application/json",
          ErrorBody(grpc::StatusCode::NOT_FOUND, "route not found", request_id)};
}

}  // namespace

struct Server::Impl {
  Config config;
  Client& client;
  audit::Service* audit_service;
  asio::io_context io;
  tcp::acceptor acceptor{io};
  std::atomic_bool is_listening{false};
  std::atomic_bool stopping{false};
  std::atomic_size_t unary_inflight{0};
  std::atomic_size_t sse_inflight{0};
  std::atomic_size_t monitor_inflight{0};
  std::atomic_int upstream_state{-1};
  std::mutex state_mutex;
  std::set<grpc::ClientContext*> contexts;
  std::set<std::shared_ptr<beast::tcp_stream>> streams;
  struct Worker {
    std::thread thread;
    std::shared_ptr<std::atomic_bool> done;
  };
  std::vector<Worker> workers;

  Impl(Config value, Client& shared_client, audit::Service* audit)
      : config(std::move(value)), client(shared_client), audit_service(audit) {
    workers.reserve(config.max_connections);
  }

  class AdmissionPermit {
   public:
    AdmissionPermit() = default;
    AdmissionPermit(const AdmissionPermit&) = delete;
    AdmissionPermit& operator=(const AdmissionPermit&) = delete;
    AdmissionPermit(AdmissionPermit&& other) noexcept
        : counter_(std::exchange(other.counter_, nullptr)) {}
    AdmissionPermit& operator=(AdmissionPermit&& other) noexcept {
      if (this == &other) return *this;
      Release();
      counter_ = std::exchange(other.counter_, nullptr);
      return *this;
    }
    ~AdmissionPermit() { Release(); }

    explicit operator bool() const { return counter_ != nullptr; }

    static AdmissionPermit TryAcquire(std::atomic_size_t& counter, std::size_t limit) {
      std::size_t current = counter.load(std::memory_order_relaxed);
      while (current < limit) {
        if (counter.compare_exchange_weak(current, current + 1, std::memory_order_acquire,
                                          std::memory_order_relaxed))
          return AdmissionPermit(&counter);
      }
      return {};
    }

   private:
    explicit AdmissionPermit(std::atomic_size_t* counter) : counter_(counter) {}
    void Release() {
      if (counter_ != nullptr) {
        counter_->fetch_sub(1, std::memory_order_release);
        counter_ = nullptr;
      }
    }

    std::atomic_size_t* counter_{nullptr};
  };

  class HelperThread {
   public:
    template <typename Function>
    HelperThread(std::atomic_bool& done, std::condition_variable* wakeup, Function&& function)
        : done_(done), wakeup_(wakeup), thread_(std::forward<Function>(function)) {}
    template <typename Function, typename Unblock>
    HelperThread(std::atomic_bool& done, std::condition_variable* wakeup, Function&& function,
                 Unblock&& unblock)
        : done_(done),
          wakeup_(wakeup),
          unblock_(std::forward<Unblock>(unblock)),
          thread_(std::forward<Function>(function)) {}
    HelperThread(const HelperThread&) = delete;
    HelperThread& operator=(const HelperThread&) = delete;
    ~HelperThread() { StopAndJoin(); }

    void StopAndJoin() {
      done_.store(true, std::memory_order_release);
      if (wakeup_ != nullptr) wakeup_->notify_all();
      if (unblock_) unblock_();
      if (thread_.joinable()) thread_.join();
    }

   private:
    std::atomic_bool& done_;
    std::condition_variable* wakeup_;
    std::function<void()> unblock_;
    std::thread thread_;
  };

  struct ContextRegistration {
    Impl& impl;
    grpc::ClientContext& context;
    ContextRegistration(Impl& owner, grpc::ClientContext& value) : impl(owner), context(value) {
      std::lock_guard<std::mutex> lock(impl.state_mutex);
      impl.contexts.insert(&context);
    }
    ~ContextRegistration() {
      std::lock_guard<std::mutex> lock(impl.state_mutex);
      impl.contexts.erase(&context);
    }
  };

  void Prepare(grpc::ClientContext* context, const std::string& request_id, int timeout_ms) {
    context->AddMetadata(eos::lib::kRequestIdMetadataKey, request_id);
    context->set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(timeout_ms));
  }

  void PrepareStream(grpc::ClientContext* context, const std::string& request_id) {
    context->AddMetadata(eos::lib::kRequestIdMetadataKey, request_id);
  }

  bool OriginAllowed(const std::string& origin) const {
    return std::find(config.allowed_origins.begin(), config.allowed_origins.end(), origin) !=
           config.allowed_origins.end();
  }

  void ObserveUpstream(bool ready, const char* phase) {
    const int next = ready ? 1 : 0;
    const int previous = upstream_state.exchange(next);
    if (previous == next) return;
    const char* event =
        ready ? (previous == 0 ? "north_http_upstream_reconnected" : "north_http_upstream_ready")
              : "north_http_upstream_unavailable";
    eos::lib::LogEvent(Logger(), ready ? spdlog::level::info : spdlog::level::warn, event,
                       {{"upstream", config.upstream}, {"phase", phase}});
  }

  bool Ready(const std::string& request_id) {
    grpc::ClientContext context;
    Prepare(&context, request_id, config.connect_timeout_ms);
    ContextRegistration registration(*this, context);
    grpc::GenericStub health(client.channel());
    grpc::CompletionQueue queue;
    const grpc::Slice empty_slice("", 0);
    const grpc::ByteBuffer request(&empty_slice, 1);
    grpc::ByteBuffer response;
    auto call = health.PrepareUnaryCall(&context, "/grpc.health.v1.Health/Check", request, &queue);
    if (!call) {
      ObserveUpstream(false, "health_prepare");
      return false;
    }
    call->StartCall();
    grpc::Status status;
    call->Finish(&response, &status, reinterpret_cast<void*>(1));
    void* tag = nullptr;
    bool ok = false;
    if (!queue.Next(&tag, &ok) || tag != reinterpret_cast<void*>(1) || !ok || !status.ok()) {
      eos::lib::LogEvent(Logger(), spdlog::level::warn, "north_http_health_check_failed",
                         {{"grpc_status", static_cast<int>(status.error_code())},
                          {"grpc_message", status.error_message()},
                          {"completion_ok", ok}});
      ObserveUpstream(false, "health_check");
      return false;
    }
    std::vector<grpc::Slice> slices;
    if (!response.Dump(&slices).ok()) {
      ObserveUpstream(false, "health_decode");
      return false;
    }
    std::string encoded;
    for (const auto& slice : slices)
      encoded.append(reinterpret_cast<const char*>(slice.begin()), slice.size());
    const bool ready = encoded == std::string("\x08\x01", 2);
    ObserveUpstream(ready, "health_check");
    return ready;
  }

  template <typename Request, typename Response, typename Call>
  Reply Unary(const Request& request, const std::string& request_id, Call call) {
    grpc::ClientContext context;
    Prepare(&context, request_id, config.request_timeout_ms);
    ContextRegistration registration(*this, context);
    Response response;
    const grpc::Status status = call(&context, request, &response);
    if (!status.ok()) {
      if (status.error_code() == grpc::StatusCode::UNAVAILABLE)
        ObserveUpstream(false, "unary_call");
      return {HttpStatus(status.error_code()), "application/json",
              ErrorBody(status.error_code(), status.error_message(), request_id)};
    }
    ObserveUpstream(true, "unary_call");
    Reply reply{http::status::ok, "application/json", MessageJson(response)};
    const auto metadata = context.GetServerInitialMetadata();
    const auto sample_age = metadata.find("eos-sample-age-ms");
    if (sample_age != metadata.end())
      reply.sample_age_ms.assign(sample_age->second.data(), sample_age->second.length());
    return reply;
  }

  Reply Route(http::verb method, const Target& target, const std::string& body,
              const std::string& request_id) {
    google::protobuf::Empty empty;
    if (method == http::verb::get && target.path == "/health/live") {
      return {http::status::ok, "application/json", "{\"status\":\"live\"}"};
    }
    if (method == http::verb::get && target.path == "/health/ready") {
      if (!Ready(request_id))
        return {
            http::status::service_unavailable, "application/json",
            ErrorBody(grpc::StatusCode::UNAVAILABLE, "EOS gRPC upstream is not ready", request_id)};
      return {http::status::ok, "application/json", "{\"status\":\"ready\"}"};
    }
    if (method == http::verb::get && target.path == "/v1/eos/audit/status") {
      if (audit_service == nullptr) {
        return {http::status::ok, "application/json",
                json{{"enabled", false}, {"health", "disabled"}, {"running", false}}.dump(), true};
      }
      const audit::Status status = audit_service->status();
      return {http::status::ok, "application/json",
              json{{"enabled", true},
                   {"health", audit::ToString(status.health)},
                   {"error", audit::ToString(status.error)},
                   {"running", status.running},
                   {"cursor", json{{"epoch", status.epoch}, {"sequence", status.sequence}}},
                   {"records", status.records}}
                  .dump(),
              true};
    }
    if (method == http::verb::get && target.path == "/v1/eos/audit/action-lists") {
      if (audit_service == nullptr) {
        return {http::status::ok, "application/json",
                json{{"enabled", false}, {"records", json::array()}, {"truncated", false}}.dump(),
                true};
      }
      std::size_t limit = 100;
      const auto found = target.query.find("limit");
      if (found != target.query.end()) {
        std::string error;
        const std::uint64_t parsed = ParseSequence(found->second, "limit", &error);
        if (!error.empty() || parsed == 0 || parsed > 1000) {
          Reply rejected = BadRequest("limit must be an integer in [1, 1000]", request_id);
          rejected.no_store = true;
          return rejected;
        }
        limit = static_cast<std::size_t>(parsed);
      }
      audit::RecordPage page = audit_service->record_page(limit);
      if (page.oversized) {
        return {HttpStatus(grpc::StatusCode::RESOURCE_EXHAUSTED), "application/json",
                ErrorBody(grpc::StatusCode::RESOURCE_EXHAUSTED,
                          "newest audit record exceeds response byte limit", request_id),
                true};
      }
      return {http::status::ok, "application/json",
              json{{"enabled", true},
                   {"records", std::move(page.records)},
                   {"truncated", page.truncated}}
                  .dump(),
              true};
    }
    if (method == http::verb::get && target.path == "/v1/eos/discover") {
      return Unary<google::protobuf::Empty, island::eos::v1::DiscoverResponse>(
          empty, request_id,
          [this](auto* c, const auto& r, auto* o) { return client.gateway().Discover(c, r, o); });
    }
    if (method == http::verb::get && target.path == "/v1/eos/status") {
      return Unary<google::protobuf::Empty, google::protobuf::Struct>(
          empty, request_id,
          [this](auto* c, const auto& r, auto* o) { return client.gateway().GetStatus(c, r, o); });
    }
    const auto& model_path = target.segments;
    if (method == http::verb::get && model_path.size() == 5 && model_path[0] == "v1" &&
        model_path[1] == "eos" && model_path[2] == "models" &&
        model_path[4] == "status") {
      island::eos::v1::ModelStatusRequest request;
      request.set_model_id(model_path[3]);
      return Unary<island::eos::v1::ModelStatusRequest, island::eos::v1::ModelStatus>(
          request, request_id, [this](auto* c, const auto& r, auto* o) {
            return client.gateway().GetModelStatus(c, r, o);
          });
    }
    if (method == http::verb::get && target.path == "/v1/eos/resources") {
      return Unary<google::protobuf::Empty, google::protobuf::ListValue>(
          empty, request_id, [this](auto* c, const auto& r, auto* o) {
            return client.gateway().ListResources(c, r, o);
          });
    }
    if (method == http::verb::get && target.path == "/v1/eos/resource") {
      island::eos::v1::ResourceRequest request;
      const auto found = target.query.find("uri");
      if (found == target.query.end())
        return BadRequest("uri query parameter is required", request_id);
      request.set_uri(found->second);
      return Unary<island::eos::v1::ResourceRequest, google::protobuf::Struct>(
          request, request_id, [this](auto* c, const auto& r, auto* o) {
            return client.gateway().ReadResource(c, r, o);
          });
    }
    if (method == http::verb::post && target.path == "/v1/eos/navigation/waypoints:reload") {
      return Unary<google::protobuf::Empty, island::eos::v1::ReloadNavigationPointsResponse>(
          empty, request_id, [this](auto* c, const auto& r, auto* o) {
            return client.gateway().ReloadNavigationPoints(c, r, o);
          });
    }
    const auto& nav = target.segments;
    const bool nav_collection = nav.size() == 6 && nav[0] == "v1" && nav[1] == "eos" &&
                                nav[2] == "robots" && nav[4] == "navigation" &&
                                nav[5] == "waypoints";
    if (nav_collection && method == http::verb::get) {
      island::eos::v1::ListNavigationPointsRequest request;
      request.set_robot_name(nav[3]);
      return Unary<island::eos::v1::ListNavigationPointsRequest,
                   island::eos::v1::ListNavigationPointsResponse>(
          request, request_id, [this](auto* c, const auto& r, auto* o) {
            return client.gateway().ListNavigationPoints(c, r, o);
          });
    }
    const bool nav_item = nav.size() == 7 && nav[0] == "v1" && nav[1] == "eos" &&
                          nav[2] == "robots" && nav[4] == "navigation" &&
                          nav[5] == "waypoints";
    if (nav_item && method == http::verb::get) {
      island::eos::v1::GetNavigationPointRequest request;
      request.set_robot_name(nav[3]);
      request.set_name(nav[6]);
      return Unary<island::eos::v1::GetNavigationPointRequest, island::eos::v1::NavigationPoint>(
          request, request_id, [this](auto* c, const auto& r, auto* o) {
            return client.gateway().GetNavigationPoint(c, r, o);
          });
    }
    if (nav_item && method == http::verb::put) {
      island::eos::v1::PutNavigationPointRequest request;
      std::string error;
      if (!ParseJson(body, &request, &error)) return BadRequest(error, request_id);
      if (!request.has_x_m() || !request.has_y_m() || !request.has_yaw_rad())
        return BadRequest("x_m, y_m, and yaw_rad are required", request_id);
      if ((!request.robot_name().empty() && request.robot_name() != nav[3]) ||
          (!request.name().empty() && request.name() != nav[6]))
        return BadRequest("body robot_name/name must match the request path", request_id);
      request.set_robot_name(nav[3]);
      request.set_name(nav[6]);
      return Unary<island::eos::v1::PutNavigationPointRequest, island::eos::v1::NavigationPoint>(
          request, request_id, [this](auto* c, const auto& r, auto* o) {
            return client.gateway().PutNavigationPoint(c, r, o);
          });
    }
    if (nav_item && method == http::verb::delete_) {
      island::eos::v1::GetNavigationPointRequest request;
      request.set_robot_name(nav[3]);
      request.set_name(nav[6]);
      return Unary<island::eos::v1::GetNavigationPointRequest, google::protobuf::Empty>(
          request, request_id, [this](auto* c, const auto& r, auto* o) {
            return client.gateway().DeleteNavigationPoint(c, r, o);
          });
    }
    if (method == http::verb::post && target.path == "/v1/eos/resource:history") {
      island::eos::v1::QueryResourceHistoryRequest request;
      std::string error;
      if (!ParseJson(body, &request, &error)) return BadRequest(error, request_id);
      return Unary<island::eos::v1::QueryResourceHistoryRequest,
                   island::eos::v1::QueryResourceHistoryResponse>(
          request, request_id, [this](auto* c, const auto& r, auto* o) {
            return client.gateway().QueryResourceHistory(c, r, o);
          });
    }
    if (method == http::verb::post && target.path == "/v1/eos/primitives:call") {
      island::eos::v1::CallPrimitiveRequest request;
      std::string error;
      if (!ParseJson(body, &request, &error)) return BadRequest(error, request_id);
      return Unary<island::eos::v1::CallPrimitiveRequest, google::protobuf::Struct>(
          request, request_id, [this](auto* c, const auto& r, auto* o) {
            return client.gateway().CallPrimitive(c, r, o);
          });
    }
    if (method == http::verb::post && target.path == "/v1/eos/motions:cancel") {
      island::eos::v1::PrimitiveCommandRequest request;
      std::string error;
      if (!ParseJson(body, &request, &error)) return BadRequest(error, request_id);
      return Unary<island::eos::v1::PrimitiveCommandRequest, google::protobuf::Struct>(
          request, request_id, [this](auto* c, const auto& r, auto* o) {
            return client.gateway().CancelPrimitive(c, r, o);
          });
    }
    if (method == http::verb::post && target.path == "/v1/eos/world/camera:capture") {
      island::eos::v1::CaptureFramesRequest request;
      std::string error;
      if (!ParseJson(body, &request, &error)) return BadRequest(error, request_id);
      return Unary<island::eos::v1::CaptureFramesRequest, island::eos::v1::CaptureFramesResponse>(
          request, request_id, [this](auto* c, const auto& r, auto* o) {
            return client.gateway().CaptureFrames(c, r, o);
           });
    }
    if (method == http::verb::post && target.path == "/v1/eos/perception/objects:observe") {
      island::eos::v1::ObserveObjectsRequest request;
      std::string error;
      if (!ParseJson(body, &request, &error)) return BadRequest(error, request_id);
      return Unary<island::eos::v1::ObserveObjectsRequest,
                   island::eos::v1::ObserveObjectsResponse>(
          request, request_id, [this](auto* c, const auto& r, auto* o) {
            return client.gateway().ObserveObjects(c, r, o);
          });
    }
    if (method == http::verb::get && target.path == "/v1/eos/perception/roles") {
      google::protobuf::Empty request;
      return Unary<google::protobuf::Empty, island::eos::v1::PerceptionRolesResponse>(
          request, request_id, [this](auto* c, const auto& r, auto* o) {
            return client.gateway().GetPerceptionRoles(c, r, o);
          });
    }
    if (method == http::verb::post && target.path == "/v1/eos/perception/vad:infer") {
      island::eos::v1::InferVadRequest request;
      std::string error;
      if (!ParseJson(body, &request, &error)) return BadRequest(error, request_id);
      return Unary<island::eos::v1::InferVadRequest, island::eos::v1::InferVadResponse>(
          request, request_id,
          [this](auto* c, const auto& r, auto* o) { return client.gateway().InferVad(c, r, o); });
    }
    if (method == http::verb::post && target.path == "/v1/eos/perception/humans:detect") {
      island::eos::v1::DetectHumansRequest request;
      std::string error;
      if (!ParseJson(body, &request, &error)) return BadRequest(error, request_id);
      return Unary<island::eos::v1::DetectHumansRequest,
                   island::eos::v1::DetectHumansResponse>(
          request, request_id, [this](auto* c, const auto& r, auto* o) {
            return client.gateway().DetectHumans(c, r, o);
          });
    }
    if (method == http::verb::post &&
        target.path == "/v1/eos/perception/human-poses:estimate") {
      island::eos::v1::EstimateHumanPoseRequest request;
      std::string error;
      if (!ParseJson(body, &request, &error)) return BadRequest(error, request_id);
      return Unary<island::eos::v1::EstimateHumanPoseRequest,
                   island::eos::v1::EstimateHumanPoseResponse>(
          request, request_id, [this](auto* c, const auto& r, auto* o) {
            return client.gateway().EstimateHumanPose(c, r, o);
          });
    }
    if (method == http::verb::post && target.path == "/v1/eos/perception/faces:detect") {
      island::eos::v1::DetectFacesRequest request;
      std::string error;
      if (!ParseJson(body, &request, &error)) return BadRequest(error, request_id);
      return Unary<island::eos::v1::DetectFacesRequest, island::eos::v1::DetectFacesResponse>(
          request, request_id, [this](auto* c, const auto& r, auto* o) {
            return client.gateway().DetectFaces(c, r, o);
          });
    }

    const auto& part = target.segments;
    if (method == http::verb::get && target.path == "/v1/eos/skills") {
      island::eos::v1::ListSkillsRequest request;
      return Unary<island::eos::v1::ListSkillsRequest, island::eos::v1::ListSkillsResponse>(
          request, request_id,
          [this](auto* c, const auto& r, auto* o) { return client.skills().ListSkills(c, r, o); });
    }
    if (part.size() == 7 && part[0] == "v1" && part[1] == "eos" && part[2] == "skills" &&
        part[4] == "versions") {
      island::eos::v1::SkillReference reference;
      reference.set_name(part[3]);
      reference.set_version(part[5]);
      if (method == http::verb::get && part[6].empty()) return NotFound(request_id);
      if (method == http::verb::post && part[6] == "executions") {
        island::eos::v1::StartExecutionRequest request;
        std::string error;
        if (!ParseJson(body, &request, &error)) return BadRequest(error, request_id);
        if (request.has_reference() &&
            ((!request.reference().name().empty() && request.reference().name() != part[3]) ||
             (!request.reference().version().empty() &&
              request.reference().version() != part[5]))) {
          return BadRequest("body reference does not match skill path", request_id);
        }
        *request.mutable_reference() = reference;
        return Unary<island::eos::v1::StartExecutionRequest, island::eos::v1::SkillExecution>(
            request, request_id, [this](auto* c, const auto& r, auto* o) {
              return client.skills().StartExecution(c, r, o);
            });
      }
    }
    if (part.size() == 6 && part[0] == "v1" && part[1] == "eos" && part[2] == "skills" &&
        part[4] == "versions" && method == http::verb::get) {
      island::eos::v1::GetSkillRequest request;
      request.mutable_reference()->set_name(part[3]);
      request.mutable_reference()->set_version(part[5]);
      return Unary<island::eos::v1::GetSkillRequest, island::eos::v1::SkillDescriptor>(
          request, request_id,
          [this](auto* c, const auto& r, auto* o) { return client.skills().GetSkill(c, r, o); });
    }
    if (method == http::verb::get && target.path == "/v1/eos/executions") {
      island::eos::v1::ListExecutionsRequest request;
      return Unary<island::eos::v1::ListExecutionsRequest, island::eos::v1::ListExecutionsResponse>(
          request, request_id, [this](auto* c, const auto& r, auto* o) {
            return client.skills().ListExecutions(c, r, o);
          });
    }
    if (method == http::verb::post && target.path == "/v1/eos/executions:terminate") {
      island::eos::v1::TerminateExecutionsRequest request;
      std::string error;
      if (!ParseJson(body, &request, &error)) return BadRequest(error, request_id);
      return Unary<island::eos::v1::TerminateExecutionsRequest,
                   island::eos::v1::TerminateExecutionsResponse>(
          request, request_id, [this](auto* c, const auto& r, auto* o) {
            return client.skills().TerminateExecutions(c, r, o);
          });
    }
    if (part.size() == 4 && part[0] == "v1" && part[1] == "eos" && part[2] == "executions") {
      const std::string& operation = part[3];
      const std::size_t colon = operation.find(':');
      const std::string id = operation.substr(0, colon);
      const std::string suffix = colon == std::string::npos ? "" : operation.substr(colon + 1);
      if (method == http::verb::get && suffix.empty()) {
        island::eos::v1::GetExecutionRequest request;
        request.set_execution_id(id);
        return Unary<island::eos::v1::GetExecutionRequest, island::eos::v1::SkillExecution>(
            request, request_id, [this](auto* c, const auto& r, auto* o) {
              return client.skills().GetExecution(c, r, o);
            });
      }
      if (method == http::verb::post && suffix == "terminate") {
        island::eos::v1::TerminateExecutionRequest request;
        std::string error;
        if (!body.empty() && body != "{}" && !ParseJson(body, &request, &error))
          return BadRequest(error, request_id);
        if (!request.execution_id().empty() && request.execution_id() != id)
          return BadRequest("body executionId does not match path", request_id);
        request.set_execution_id(id);
        return Unary<island::eos::v1::TerminateExecutionRequest, island::eos::v1::SkillExecution>(
            request, request_id, [this](auto* c, const auto& r, auto* o) {
              return client.skills().TerminateExecution(c, r, o);
            });
      }
      if (method == http::verb::post && suffix == "signal") {
        island::eos::v1::SignalExecutionRequest request;
        std::string error;
        if (!ParseJson(body, &request, &error)) return BadRequest(error, request_id);
        if (!request.execution_id().empty() && request.execution_id() != id)
          return BadRequest("body executionId does not match path", request_id);
        request.set_execution_id(id);
        return Unary<island::eos::v1::SignalExecutionRequest, island::eos::v1::SkillSignalReceipt>(
            request, request_id, [this](auto* c, const auto& r, auto* o) {
              return client.skills().SignalExecution(c, r, o);
            });
      }
    }
    if (part.size() == 5 && part[0] == "v1" && part[1] == "eos" &&
        part[2] == "executions" && part[4] == "state" && method == http::verb::get) {
      island::eos::v1::GetExecutionStateRequest request;
      request.set_execution_id(part[3]);
      return Unary<island::eos::v1::GetExecutionStateRequest,
                   island::eos::v1::SkillExecutionState>(
          request, request_id, [this](auto* c, const auto& r, auto* o) {
            return client.skills().GetExecutionState(c, r, o);
          });
    }
    if (method == http::verb::get && target.path == "/v1/eos/workflow-templates") {
      island::eos::v1::ListWorkflowTemplatesRequest request;
      return Unary<island::eos::v1::ListWorkflowTemplatesRequest,
                   island::eos::v1::ListWorkflowTemplatesResponse>(
          request, request_id, [this](auto* c, const auto& r, auto* o) {
            return client.workflows().ListWorkflowTemplates(c, r, o);
          });
    }
    if (part.size() == 4 && part[0] == "v1" && part[1] == "eos" &&
        part[2] == "workflow-templates") {
      const std::string& operation = part[3];
      const std::size_t colon = operation.find(':');
      const std::string name = operation.substr(0, colon);
      const std::string suffix = colon == std::string::npos ? "" : operation.substr(colon + 1);
      if (method == http::verb::get && suffix.empty()) {
        island::eos::v1::GetWorkflowTemplateRequest request;
        request.set_name(name);
        return Unary<island::eos::v1::GetWorkflowTemplateRequest,
                     island::eos::v1::WorkflowTemplateDescriptor>(
            request, request_id, [this](auto* c, const auto& r, auto* o) {
              return client.workflows().GetWorkflowTemplate(c, r, o);
            });
      }
      if (method == http::verb::post && suffix == "run") {
        island::eos::v1::RunWorkflowTemplateRequest request;
        std::string error;
        if (!body.empty() && body != "{}" && !ParseJson(body, &request, &error))
          return BadRequest(error, request_id);
        if (!request.name().empty() && request.name() != name)
          return BadRequest("body name does not match workflow template path", request_id);
        request.set_name(name);
        return Unary<island::eos::v1::RunWorkflowTemplateRequest,
                     island::eos::v1::ActionList>(
            request, request_id, [this](auto* c, const auto& r, auto* o) {
              return client.workflows().RunWorkflowTemplate(c, r, o);
            });
      }
    }
    if (method == http::verb::post && target.path == "/v1/eos/workflows") {
      island::eos::v1::ApplyActionListRequest request;
      std::string error;
      if (!ParseJson(body, &request, &error)) return BadRequest(error, request_id);
      return Unary<island::eos::v1::ApplyActionListRequest, island::eos::v1::ActionList>(
          request, request_id, [this](auto* c, const auto& r, auto* o) {
            return client.workflows().ApplyActionList(c, r, o);
          });
    }
    if (method == http::verb::get && target.path == "/v1/eos/workflows") {
      island::eos::v1::ListActionListsRequest request;
      return Unary<island::eos::v1::ListActionListsRequest,
                   island::eos::v1::ListActionListsResponse>(
          request, request_id, [this](auto* c, const auto& r, auto* o) {
            return client.workflows().ListActionLists(c, r, o);
          });
    }
    if (part.size() == 4 && part[0] == "v1" && part[1] == "eos" && part[2] == "workflows" &&
        method == http::verb::get) {
      island::eos::v1::GetActionListRequest request;
      request.set_workflow_id(part[3]);
      return Unary<island::eos::v1::GetActionListRequest, island::eos::v1::ActionList>(
          request, request_id, [this](auto* c, const auto& r, auto* o) {
            return client.workflows().GetActionList(c, r, o);
          });
    }
    if (method == http::verb::get && target.path == "/v1/eos/runtime:snapshot") {
      island::eos::v1::GetRuntimeSnapshotRequest request;
      return Unary<island::eos::v1::GetRuntimeSnapshotRequest,
                   island::eos::v1::ManagerRuntimeSnapshot>(
          request, request_id, [this](auto* c, const auto& r, auto* o) {
            return client.workflows().GetRuntimeSnapshot(c, r, o);
          });
    }
    return NotFound(request_id);
  }

  bool IsStreamRoute(const Target& target) const {
    if (target.path == "/v1/eos/resource:watch") return true;
    if (target.path == "/v1/eos/workflows:watch" ||
        target.path == "/v1/eos/executions:monitor")
      return true;
    const auto& part = target.segments;
    if (part.size() == 5 && part[0] == "v1" && part[1] == "eos" && part[2] == "models" &&
        part[4] == "status:watch")
      return true;
    const auto is_watch = [](const std::string& value) {
      constexpr std::string_view suffix = ":watch";
      return value.size() > suffix.size() &&
             value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    if (part.size() == 4 && part[0] == "v1" && part[1] == "eos" && part[2] == "executions" &&
        is_watch(part[3]))
      return true;
    return part.size() == 5 && part[0] == "v1" && part[1] == "eos" && part[2] == "executions" &&
           (part[4] == "journal:follow" || part[4] == "state:watch");
  }

  bool IsMonitorWebSocket(const http::request<http::string_body>& request,
                          const Target& target) const {
    return request.method() == http::verb::get && target.path == "/v1/eos/monitor/ws" &&
           websocket::is_upgrade(request);
  }

  static json MonitorError(const std::string& code, const std::string& message,
                           const std::string& request_id, int version = 1) {
    return {{"version", version}, {"type", "monitor.error"}, {"code", code},
            {"message", message}, {"request_id", request_id}};
  }

  void MonitorWebSocket(const http::request<http::string_body>& request,
                        const std::shared_ptr<beast::tcp_stream>& stream,
                        const std::string& request_id) {
    const auto started = std::chrono::steady_clock::now();
    std::size_t camera_frames = 0;
    std::size_t audio_events = 0;
    std::size_t response_bytes = 0;
    std::atomic_bool client_disconnected{false};
    grpc::Status status;
    websocket::stream<beast::tcp_stream&> socket(*stream);
    socket.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
    socket.read_message_max(config.max_request_bytes);
    socket.set_option(websocket::stream_base::decorator([&](websocket::response_type& response) {
      response.set(http::field::server, "eos-north-http");
      response.set("X-Request-ID", request_id);
      const auto origin = request.find(http::field::origin);
      if (origin != request.end())
        response.set(http::field::access_control_allow_origin, origin->value());
    }));
    beast::error_code error;
    socket.accept(request, error);
    if (error) {
      eos::lib::LogEvent(Logger(), spdlog::level::warn, "north_http_monitor_transport_error",
                         {{"operation", "accept"}, {"error", error.message()}});
      return;
    }

    auto write_text = [&](const json& payload) {
      const std::string encoded = payload.dump();
      socket.text(true);
      socket.write(asio::buffer(encoded), error);
      if (!error) response_bytes += encoded.size();
      return !error;
    };
    auto reject = [&](const std::string& code, const std::string& message) {
      write_text(MonitorError(code, message, request_id));
      beast::error_code ignored;
      socket.close(websocket::close_code::policy_error, ignored);
    };

    beast::flat_buffer input;
    socket.read(input, error);
    if (error) return;
    if (!socket.got_text()) {
      reject("invalid_subscription", "monitor subscription must be a text JSON message");
      return;
    }
    json subscription;
    try {
      subscription = json::parse(beast::buffers_to_string(input.data()));
    } catch (const json::exception&) {
      reject("invalid_subscription", "monitor subscription must be valid JSON");
      return;
    }
    const std::set<std::string> allowed_keys{
        "version", "type", "camera_device_id", "microphone_device_id", "include_detections",
        "detection_source"};
    bool unknown_key = false;
    if (subscription.is_object()) {
      for (auto item = subscription.begin(); item != subscription.end(); ++item)
        if (allowed_keys.find(item.key()) == allowed_keys.end()) unknown_key = true;
    }
    if (!subscription.is_object() || unknown_key ||
        !subscription.contains("version") || !subscription["version"].is_number_integer() ||
        subscription["version"].get<int>() != 1 ||
        !subscription.contains("type") ||
        !subscription["type"].is_string() ||
        subscription["type"].get<std::string>() != "monitor.subscribe" ||
        !subscription.contains("camera_device_id") ||
        !subscription["camera_device_id"].is_string() ||
        subscription["camera_device_id"].get<std::string>().empty() ||
        (subscription.contains("microphone_device_id") &&
         (!subscription["microphone_device_id"].is_string() ||
          subscription["microphone_device_id"].get<std::string>().empty())) ||
        (subscription.contains("include_detections") &&
         !subscription["include_detections"].is_boolean()) ||
        (subscription.contains("detection_source") &&
         (!subscription["detection_source"].is_string() ||
          (subscription["detection_source"] != "human" &&
           subscription["detection_source"] != "object")))) {
      reject("invalid_subscription", "monitor subscription has invalid or missing fields");
      return;
    }

    island::eos::v1::MonitorRequest monitor_request;
    const int monitor_version = subscription["version"].get<int>();
    monitor_request.set_version(monitor_version);
    monitor_request.set_camera_device_id(subscription["camera_device_id"].get<std::string>());
    if (subscription.contains("microphone_device_id"))
      monitor_request.set_microphone_device_id(
          subscription["microphone_device_id"].get<std::string>());
    if (subscription.contains("include_detections"))
      monitor_request.set_include_detections(subscription["include_detections"].get<bool>());
    if (subscription.contains("detection_source"))
      monitor_request.set_detection_source(subscription["detection_source"].get<std::string>());
    monitor_request.set_max_camera_fps(30);
    grpc::ClientContext context;
    PrepareStream(&context, request_id);
    ContextRegistration registration(*this, context);
    auto reader = client.gateway().WatchMonitor(&context, monitor_request);
    std::atomic_bool done{false};
    HelperThread disconnect_monitor(done, nullptr, [&] {
      try {
        pollfd descriptor{stream->socket().native_handle(), POLLIN | POLLHUP | POLLERR, 0};
        while (!done.load(std::memory_order_acquire)) {
          const int result = ::poll(&descriptor, 1, 100);
          if (result == 0) continue;
          if (result < 0) {
            if (errno == EINTR) continue;
            break;
          }
          if ((descriptor.revents & (POLLIN | POLLHUP | POLLERR)) == 0) continue;
          char byte = 0;
          const ssize_t size =
              ::recv(descriptor.fd, &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT);
          if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
          client_disconnected.store(true);
          context.TryCancel();
          break;
        }
      } catch (...) {
        client_disconnected.store(true);
        try {
          context.TryCancel();
        } catch (...) {
        }
      }
    });

    island::eos::v1::MonitorEvent event;
    while (!error && reader->Read(&event)) {
      if (event.has_ready()) {
        const auto& ready = event.ready();
        json ready_payload{{"version", monitor_version}, {"type", "monitor.ready"},
                         {"camera_device_id", ready.camera_device_id()},
                         {"width_px", ready.camera_width_px()},
                         {"height_px", ready.camera_height_px()},
                         {"fps", ready.camera_fps()},
                         {"sample_rate_hz", ready.audio_sample_rate_hz()},
                         {"channels", ready.audio_channels()}};
        if (!ready.microphone_device_id().empty())
          ready_payload["microphone_device_id"] = ready.microphone_device_id();
        ready_payload["detection_status"] = ready.detection_status();
        if (!write_text(ready_payload))
          break;
      } else if (event.has_status()) {
        if (!write_text({{"version", monitor_version}, {"type", "monitor.status"},
                         {"state", event.status().code()},
                         {"message", event.status().message()}}))
          break;
      } else if (event.has_audio()) {
        const auto& audio = event.audio();
        json payload{{"version", monitor_version}, {"type", "monitor.audio"}, {"rms", audio.rms()},
                     {"peak", audio.peak()}, {"dbfs", audio.dbfs()},
                     {"clipping", audio.clipping()},
                     {"sequence", std::to_string(audio.sequence())},
                     {"sample_rate_hz", audio.sample_rate_hz()}, {"channels", audio.channels()},
                     {"captured_at", audio.captured_at()}};
        if (audio.has_source_timestamp_ns())
          payload["source_timestamp_ns"] = std::to_string(audio.source_timestamp_ns());
        if (!write_text(payload)) break;
        ++audio_events;
      } else if (event.has_camera()) {
        const auto& camera = event.camera();
        json metadata{{"sequence", std::to_string(camera.sequence())}, {"captured_at", camera.captured_at()},
                      {"width_px", camera.width_px()}, {"height_px", camera.height_px()},
                      {"dropped_frames", std::to_string(camera.dropped_frames())}};
        if (camera.has_source_timestamp_ns())
          metadata["source_timestamp_ns"] = std::to_string(camera.source_timestamp_ns());
        const std::string encoded = metadata.dump();
        std::vector<std::uint8_t> frame;
        frame.reserve(12 + encoded.size() + camera.jpeg().size());
        frame.insert(frame.end(), {'E', 'O', 'S', 'M', static_cast<std::uint8_t>(monitor_version), 1, 0, 0});
        const std::uint32_t length = static_cast<std::uint32_t>(encoded.size());
        frame.push_back(static_cast<std::uint8_t>((length >> 24) & 0xff));
        frame.push_back(static_cast<std::uint8_t>((length >> 16) & 0xff));
        frame.push_back(static_cast<std::uint8_t>((length >> 8) & 0xff));
        frame.push_back(static_cast<std::uint8_t>(length & 0xff));
        frame.insert(frame.end(), encoded.begin(), encoded.end());
        frame.insert(frame.end(), camera.jpeg().begin(), camera.jpeg().end());
        socket.binary(true);
        socket.write(asio::buffer(frame), error);
        if (!error) {
          response_bytes += frame.size();
          ++camera_frames;
        }
      } else if (event.has_detection()) {
        const auto& detection = event.detection();
        json metadata{{"sequence", std::to_string(detection.sequence())},
                      {"generation", std::to_string(detection.generation())},
                      {"width_px", detection.width_px()}, {"height_px", detection.height_px()},
                      {"detection", json::parse(detection.detection_json())}};
        if (detection.has_source_timestamp_ns())
          metadata["source_timestamp_ns"] = std::to_string(detection.source_timestamp_ns());
        const std::string encoded = metadata.dump();
        std::vector<std::uint8_t> frame{'E', 'O', 'S', 'M', 1, 2, 0, 0};
        const std::uint32_t length = static_cast<std::uint32_t>(encoded.size());
        frame.push_back(static_cast<std::uint8_t>((length >> 24) & 0xff));
        frame.push_back(static_cast<std::uint8_t>((length >> 16) & 0xff));
        frame.push_back(static_cast<std::uint8_t>((length >> 8) & 0xff));
        frame.push_back(static_cast<std::uint8_t>(length & 0xff));
        frame.insert(frame.end(), encoded.begin(), encoded.end());
        frame.insert(frame.end(), detection.jpeg().begin(), detection.jpeg().end());
        socket.binary(true);
        socket.write(asio::buffer(frame), error);
        if (!error) { response_bytes += frame.size(); ++camera_frames; }
      }
    }
    status = reader->Finish();
    if (!status.ok() && status.error_code() != grpc::StatusCode::CANCELLED && !error)
      write_text(MonitorError("upstream_error", status.error_message(), request_id,
                              monitor_version));
    disconnect_monitor.StopAndJoin();
    if (!error && !client_disconnected.load()) {
      beast::error_code ignored;
      socket.close(status.ok() ? websocket::close_code::normal
                               : websocket::close_code::internal_error,
                   ignored);
    }
    if (status.ok())
      ObserveUpstream(true, "monitor_stream");
    else if (status.error_code() == grpc::StatusCode::UNAVAILABLE)
      ObserveUpstream(false, "monitor_stream");
    const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
    const bool expected_disconnect = client_disconnected.load();
    eos::lib::LogEvent(Logger(), expected_disconnect ? spdlog::level::debug
                                                    : status.ok() ? spdlog::level::info
                                                                  : spdlog::level::warn,
                       "north_http_monitor_closed",
                       {{"grpc_code", static_cast<int>(status.error_code())},
                        {"duration_ms", duration_ms}, {"camera_frames", camera_frames},
                        {"audio_events", audio_events}, {"response_bytes", response_bytes},
                        {"client_disconnected", expected_disconnect}},
                       expected_disconnect ? "Monitor stream closed after client disconnect"
                                           : status.ok() ? "Monitor stream completed"
                                                         : "Monitor stream closed after EOS upstream failure");
  }

  class SseWriter {
   public:
    SseWriter(std::shared_ptr<beast::tcp_stream> stream, grpc::ClientContext& context)
        : stream_(std::move(stream)), context_(context) {}

    bool Write(const std::string& value) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (failed_.load()) return false;
      beast::error_code error;
      asio::write(stream_->socket(), asio::buffer(value), error);
      if (!error) {
        bytes_written_.fetch_add(value.size());
        return true;
      }
      failed_.store(true);
      context_.TryCancel();
      return false;
    }
    bool failed() const { return failed_.load(); }
    std::size_t bytes_written() const { return bytes_written_.load(); }

   private:
    std::shared_ptr<beast::tcp_stream> stream_;
    grpc::ClientContext& context_;
    std::mutex mutex_;
    std::atomic_bool failed_{false};
    std::atomic_size_t bytes_written_{0};
  };

  template <typename Message, typename ReaderFactory, typename Id>
  grpc::Status StreamMessages(std::shared_ptr<beast::tcp_stream> stream,
                              grpc::ClientContext* context, const std::string& event,
                              const std::string& rpc, ReaderFactory factory, Id id) {
    const auto started = std::chrono::steady_clock::now();
    auto reader = factory();
    SseWriter writer(stream, *context);
    std::size_t message_count = 0;
    std::atomic_bool done{false};
    std::atomic_bool client_disconnected{false};
    auto unblock_helpers = [&] {
      try {
        context->TryCancel();
      } catch (...) {
      }
      beast::error_code ignored;
      stream->socket().cancel(ignored);
      stream->socket().shutdown(tcp::socket::shutdown_both, ignored);
      stream->socket().close(ignored);
    };
    HelperThread disconnect_monitor(done, nullptr, [&] {
      try {
        pollfd descriptor{stream->socket().native_handle(), POLLIN | POLLHUP | POLLERR, 0};
        while (!done.load(std::memory_order_acquire)) {
          const int result = ::poll(&descriptor, 1, 100);
          if (result == 0) continue;
          if (result < 0) {
            if (errno == EINTR) continue;
            break;
          }
          if ((descriptor.revents & (POLLIN | POLLHUP | POLLERR)) == 0) continue;
          char byte = 0;
          const ssize_t size =
              ::recv(descriptor.fd, &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT);
          if (size > 0 || (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) continue;
          client_disconnected.store(true);
          context->TryCancel();
          break;
        }
      } catch (...) {
        client_disconnected.store(true);
        try {
          context->TryCancel();
        } catch (...) {
        }
      }
    }, unblock_helpers);
    std::mutex heartbeat_mutex;
    std::condition_variable heartbeat_wakeup;
    HelperThread heartbeat(done, &heartbeat_wakeup, [&] {
      try {
        std::unique_lock<std::mutex> lock(heartbeat_mutex);
        while (!heartbeat_wakeup.wait_for(
            lock, std::chrono::milliseconds(config.sse_heartbeat_ms),
            [&] { return done.load(std::memory_order_acquire); })) {
          if (!writer.Write(": heartbeat\n\n")) break;
        }
      } catch (...) {
        try {
          context->TryCancel();
        } catch (...) {
        }
      }
    }, unblock_helpers);
    Message message;
    while (!writer.failed() && reader->Read(&message)) {
      std::string payload = "event: " + event + "\n";
      const std::string sequence = id(message);
      if (!sequence.empty()) payload += "id: " + sequence + "\n";
      payload += "data: " + MessageJson(message) + "\n\n";
      if (!writer.Write(payload)) break;
      ++message_count;
    }
    const grpc::Status status = reader->Finish();
    if (!status.ok() && status.error_code() != grpc::StatusCode::CANCELLED && !writer.failed()) {
      writer.Write(
          "event: error\ndata: " +
          ErrorBody(status.error_code(), status.error_message(), eos::lib::CurrentCorrelationId()) +
          "\n\n");
    }
    heartbeat.StopAndJoin();
    disconnect_monitor.StopAndJoin();
    if (status.ok())
      ObserveUpstream(true, "stream_call");
    else if (status.error_code() == grpc::StatusCode::UNAVAILABLE)
      ObserveUpstream(false, "stream_call");
    const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
    const bool client_closed = client_disconnected.load() || writer.failed();
    const bool expected_cancellation =
        client_closed && status.error_code() == grpc::StatusCode::CANCELLED;
    eos::lib::LogEvent(Logger(), expected_cancellation ? spdlog::level::debug
                                                       : status.ok() ? spdlog::level::info
                                                                     : spdlog::level::warn,
                        "north_http_upstream_stream_closed",
                       {{"rpc", rpc},
                        {"event_type", event},
                        {"grpc_code", static_cast<int>(status.error_code())},
                        {"duration_ms", duration_ms},
                        {"message_count", message_count},
                        {"response_bytes", writer.bytes_written()},
                         {"client_disconnected", client_closed},
                        {"cancelled", status.error_code() == grpc::StatusCode::CANCELLED}});
    return status;
  }

  void WriteSseHeader(const http::request<http::string_body>& request,
                      const std::shared_ptr<beast::tcp_stream>& stream,
                      const std::string& request_id) {
    http::response<http::empty_body> response(http::status::ok, request.version());
    response.set(http::field::server, "eos-north-http");
    response.set(http::field::content_type, "text/event-stream");
    response.set(http::field::cache_control, "no-cache");
    response.set(http::field::connection, "close");
    response.set("X-Accel-Buffering", "no");
    response.set("X-Request-ID", request_id);
    response.set(http::field::vary, "Origin");
    const auto origin = request.find(http::field::origin);
    if (origin != request.end() && OriginAllowed(std::string(origin->value()))) {
      response.set(http::field::access_control_allow_origin, origin->value());
      response.set(http::field::access_control_expose_headers, "X-Request-ID");
    }
    response.keep_alive(false);
    http::response_serializer<http::empty_body> serializer(response);
    http::write_header(*stream, serializer);
  }

  void Stream(const http::request<http::string_body>& http_request, const Target& target,
              const std::shared_ptr<beast::tcp_stream>& stream, const std::string& request_id) {
    grpc::ClientContext context;
    PrepareStream(&context, request_id);
    ContextRegistration registration(*this, context);
    WriteSseHeader(http_request, stream, request_id);
    const auto& model_path = target.segments;
    if (model_path.size() == 5 && model_path[0] == "v1" && model_path[1] == "eos" &&
        model_path[2] == "models" && model_path[4] == "status:watch") {
      island::eos::v1::ModelStatusRequest request;
      request.set_model_id(model_path[3]);
      StreamMessages<island::eos::v1::ModelStatus>(
          stream, &context, "model-status", "GatewayService/WatchModelStatus",
          [&] { return client.gateway().WatchModelStatus(&context, request); },
          [](const auto&) { return std::string{}; });
      return;
    }
    if (target.path == "/v1/eos/resource:watch") {
      island::eos::v1::ResourceRequest request;
      const auto found = target.query.find("uri");
      if (found == target.query.end()) {
        SseWriter writer(stream, context);
        writer.Write("event: error\ndata: " +
                     ErrorBody(grpc::StatusCode::INVALID_ARGUMENT,
                               "uri query parameter is required", request_id) +
                     "\n\n");
        return;
      }
      request.set_uri(found->second);
      StreamMessages<island::eos::v1::ResourceChanged>(
          stream, &context, "resource-changed", "GatewayService/WatchResource",
          [&] { return client.gateway().WatchResource(&context, request); },
          [](const auto&) { return std::string{}; });
      return;
    }
    if (target.path == "/v1/eos/workflows:watch") {
      island::eos::v1::WatchActionListEventsRequest request;
      std::string cursor_text;
      const auto header = http_request.find("Last-Event-ID");
      if (header != http_request.end()) cursor_text = std::string(header->value());
      if (cursor_text.empty()) {
        const auto found = target.query.find("cursor");
        if (found != target.query.end()) cursor_text = found->second;
      }
      std::string error;
      if (cursor_text.empty()) {
        error = "cursor query parameter or Last-Event-ID is required";
      } else {
        ParseActionListCursor(cursor_text, request.mutable_after(), &error);
      }
      if (!error.empty()) {
        SseWriter writer(stream, context);
        writer.Write("event: error\ndata: " +
                     ErrorBody(grpc::StatusCode::INVALID_ARGUMENT, error, request_id) + "\n\n");
        return;
      }
      StreamMessages<island::eos::v1::ActionListEvent>(
          stream, &context, "action-list-event", "WorkflowService/WatchActionListEvents",
          [&] { return client.workflows().WatchActionListEvents(&context, request); },
          [](const auto& message) {
            return message.cursor().epoch() + ":" + std::to_string(message.cursor().sequence());
          });
      return;
    }
    if (target.path == "/v1/eos/executions:monitor") {
      island::eos::v1::MonitorExecutionsRequest request;
      auto* selector = request.mutable_selector();
      if (const auto it = target.query.find("skillName"); it != target.query.end())
        selector->mutable_reference()->set_name(it->second);
      if (const auto it = target.query.find("skillVersion"); it != target.query.end())
        selector->mutable_reference()->set_version(it->second);
      std::string error;
      if (selector->reference().name().empty() && !selector->reference().version().empty())
        error = "skillVersion requires skillName";
      if (const auto it = target.query.find("status"); it != target.query.end()) {
        std::size_t begin = 0;
        while (begin <= it->second.size() && error.empty()) {
          const auto end = it->second.find(',', begin);
          std::string value = it->second.substr(begin, end == std::string::npos ? end : end - begin);
          if (value.rfind("EXECUTION_STATUS_", 0) != 0) value = "EXECUTION_STATUS_" + value;
          island::eos::v1::ExecutionStatus status;
          if (!island::eos::v1::ExecutionStatus_Parse(value, &status) ||
              status == island::eos::v1::EXECUTION_STATUS_UNSPECIFIED)
            error = "status contains an invalid execution status";
          else
            selector->add_statuses(status);
          if (end == std::string::npos) break;
          begin = end + 1;
        }
      }
      std::string sequence;
      const auto header = http_request.find("Last-Event-ID");
      if (header != http_request.end()) sequence = std::string(header->value());
      if (sequence.empty()) {
        if (const auto it = target.query.find("afterMonitorSequence"); it != target.query.end())
          sequence = it->second;
      }
      request.set_after_monitor_sequence(
          ParseSequence(sequence, "afterMonitorSequence", &error));
      if (!error.empty()) {
        SseWriter writer(stream, context);
        writer.Write("event: error\ndata: " +
                     ErrorBody(grpc::StatusCode::INVALID_ARGUMENT, error, request_id) + "\n\n");
        return;
      }
      StreamMessages<island::eos::v1::SkillMonitorUpdate>(
          stream, &context, "skill-monitor-update", "SkillService/MonitorExecutions",
          [&] { return client.skills().MonitorExecutions(&context, request); },
          [](const auto& message) { return std::to_string(message.monitor_sequence()); });
      return;
    }
    const auto& part = target.segments;
    if (part.size() == 4) {
      const std::size_t colon = part[3].find(":watch");
      island::eos::v1::WatchExecutionRequest request;
      request.set_execution_id(part[3].substr(0, colon));
      std::string error;
      std::string sequence;
      const auto header = http_request.find("Last-Event-ID");
      if (header != http_request.end()) sequence = std::string(header->value());
      if (sequence.empty()) {
        if (const auto found = target.query.find("afterSequence"); found != target.query.end())
          sequence = found->second;
      }
      request.set_after_sequence(ParseSequence(sequence, "afterSequence", &error));
      if (!error.empty()) {
        SseWriter writer(stream, context);
        writer.Write("event: error\ndata: " +
                     ErrorBody(grpc::StatusCode::INVALID_ARGUMENT, error, request_id) + "\n\n");
        return;
      }
      StreamMessages<island::eos::v1::SkillExecution>(
          stream, &context, "skill-execution", "SkillService/WatchExecution",
          [&] { return client.skills().WatchExecution(&context, request); },
          [](const auto& message) { return std::to_string(message.sequence()); });
      return;
    }
    if (part[4] == "state:watch") {
      island::eos::v1::WatchExecutionStateRequest request;
      request.set_execution_id(part[3]);
      std::string error;
      std::string revision;
      const auto header = http_request.find("Last-Event-ID");
      if (header != http_request.end()) revision = std::string(header->value());
      if (revision.empty()) {
        if (const auto found = target.query.find("afterRevision"); found != target.query.end())
          revision = found->second;
      }
      request.set_after_revision(ParseSequence(revision, "afterRevision", &error));
      if (!error.empty()) {
        SseWriter writer(stream, context);
        writer.Write("event: error\ndata: " +
                     ErrorBody(grpc::StatusCode::INVALID_ARGUMENT, error, request_id) + "\n\n");
        return;
      }
      StreamMessages<island::eos::v1::SkillExecutionState>(
          stream, &context, "skill-execution-state", "SkillService/WatchExecutionState",
          [&] { return client.skills().WatchExecutionState(&context, request); },
          [](const auto& message) { return std::to_string(message.revision()); });
      return;
    }
    island::eos::v1::FollowExecutionJournalRequest request;
    request.set_execution_id(part[3]);
    std::string error;
    std::string sequence;
    const auto header = http_request.find("Last-Event-ID");
    if (header != http_request.end()) sequence = std::string(header->value());
    if (sequence.empty()) {
      if (const auto found = target.query.find("afterJournalSequence");
          found != target.query.end())
        sequence = found->second;
    }
    request.set_after_journal_sequence(ParseSequence(sequence, "afterJournalSequence", &error));
    if (!error.empty()) {
      SseWriter writer(stream, context);
      writer.Write("event: error\ndata: " +
                   ErrorBody(grpc::StatusCode::INVALID_ARGUMENT, error, request_id) + "\n\n");
      return;
    }
    StreamMessages<island::eos::v1::SkillJournalEntry>(
        stream, &context, "skill-journal-entry", "SkillService/FollowExecutionJournal",
        [&] { return client.skills().FollowExecutionJournal(&context, request); },
        [](const auto& message) { return std::to_string(message.journal_sequence()); });
  }

  void ApplyCommonHeaders(http::response<http::string_body>* response,
                          const http::request<http::string_body>& request,
                          const std::string& request_id) {
    response->set(http::field::server, "eos-north-http");
    response->set(http::field::vary, "Origin");
    response->set("X-Request-ID", request_id);
    const auto origin = request.find(http::field::origin);
    if (origin != request.end() && OriginAllowed(std::string(origin->value()))) {
      response->set(http::field::access_control_allow_origin, origin->value());
      response->set(http::field::access_control_allow_methods, "GET, POST, PUT, DELETE, OPTIONS");
      response->set(http::field::access_control_allow_headers,
                    "Content-Type, Last-Event-ID, X-Request-ID");
      response->set(http::field::access_control_expose_headers,
                    "X-Request-ID, Eos-Sample-Age-Ms");
    }
  }

  void ReapWorkers() {
    auto worker = workers.begin();
    while (worker != workers.end()) {
      if (!worker->done->load(std::memory_order_acquire)) {
        ++worker;
        continue;
      }
      if (worker->thread.joinable()) worker->thread.join();
      worker = workers.erase(worker);
    }
  }

  void Handle(std::shared_ptr<beast::tcp_stream> stream) {
    beast::flat_buffer buffer;
    while (!stopping.load()) {
      http::request_parser<http::string_body> parser;
      parser.body_limit(config.max_request_bytes);
      stream->expires_after(std::chrono::seconds(30));
      beast::error_code error;
      http::read_header(*stream, buffer, parser, error);
      if (error == http::error::end_of_stream || error == asio::error::operation_aborted) break;
      if (error == http::error::body_limit) {
        const RequestIdResolution resolved = ResolveHttpRequestId(parser.get().base());
        const std::string& request_id = resolved.value;
        eos::lib::CorrelationScope correlation(request_id);
        http::response<http::string_body> response(http::status::payload_too_large, 11);
        response.set(http::field::content_type, "application/json");
        response.set("X-Request-ID", request_id);
        response.keep_alive(false);
        response.body() = ErrorBody(grpc::StatusCode::RESOURCE_EXHAUSTED,
                                    "request body exceeds configured limit", request_id);
        response.prepare_payload();
        http::write(*stream, response, error);
        eos::lib::LogEvent(Logger(), spdlog::level::warn, "north_http_request_rejected",
                           {{"status", 413}, {"reason", "body_limit"}});
        break;
      }
      if (error) {
        eos::lib::LogEvent(Logger(), spdlog::level::warn, "north_http_transport_error",
                           {{"operation", "read"}, {"error", error.message()}});
        break;
      }
      const auto& request_header = parser.get();
      const RequestIdResolution resolved = ResolveHttpRequestId(request_header.base());
      eos::lib::CorrelationScope correlation(resolved.value);
      const std::string request_id = correlation.id();
      const Target target = ParseTarget(std::string(request_header.target()));
      const auto started = std::chrono::steady_clock::now();
      const bool monitor = IsMonitorWebSocket(request_header, target);
      const bool sse = request_header.method() == http::verb::get && IsStreamRoute(target);
      AdmissionPermit permit;
      const char* overload_message = nullptr;
      if (monitor) {
        permit = AdmissionPermit::TryAcquire(monitor_inflight, config.max_monitor_streams);
        overload_message = "too many active north HTTP monitor streams";
      } else if (sse) {
        permit = AdmissionPermit::TryAcquire(sse_inflight, config.max_sse_streams);
        overload_message = "too many active north HTTP SSE streams";
      } else {
        permit = AdmissionPermit::TryAcquire(unary_inflight, config.max_inflight_requests);
        overload_message = "too many active unary north HTTP requests";
      }
      if (!permit) {
        Reply reply{http::status::too_many_requests, "application/json",
                    ErrorBody(grpc::StatusCode::RESOURCE_EXHAUSTED, overload_message, request_id)};
        http::response<http::string_body> response(reply.status, request_header.version());
        response.set(http::field::content_type, reply.content_type);
        ApplyCommonHeaders(&response, request_header, request_id);
        response.keep_alive(false);
        response.body() = std::move(reply.body);
        response.prepare_payload();
        const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - started)
                                     .count();
        const std::size_t response_bytes = response.body().size();
        http::write(*stream, response, error);
        eos::lib::LogEvent(Logger(), spdlog::level::warn, "north_http_request_rejected",
                           {{"method", std::string(request_header.method_string())},
                            {"path", target.path},
                            {"status", 429},
                            {"request_bytes", 0},
                            {"response_bytes", response_bytes},
                            {"duration_ms", duration_ms}});
        break;
      }
      if (!parser.is_done()) http::read(*stream, buffer, parser, error);
      if (error == http::error::body_limit) {
        http::response<http::string_body> response(http::status::payload_too_large, 11);
        response.set(http::field::content_type, "application/json");
        response.set("X-Request-ID", request_id);
        response.keep_alive(false);
        response.body() = ErrorBody(grpc::StatusCode::RESOURCE_EXHAUSTED,
                                    "request body exceeds configured limit", request_id);
        response.prepare_payload();
        http::write(*stream, response, error);
        eos::lib::LogEvent(Logger(), spdlog::level::warn, "north_http_request_rejected",
                           {{"status", 413}, {"reason", "body_limit"}});
        break;
      }
      if (error == http::error::end_of_stream || error == asio::error::operation_aborted) break;
      if (error) {
        eos::lib::LogEvent(Logger(), spdlog::level::warn, "north_http_transport_error",
                           {{"operation", "read"}, {"error", error.message()}});
        break;
      }
      auto request = parser.release();
      Reply reply;
      bool streamed = false;
      if (!resolved.error.empty()) {
        reply = BadRequest(resolved.error, request_id);
      } else if (monitor && request.find(http::field::origin) != request.end() &&
                  !OriginAllowed(std::string(request[http::field::origin]))) {
        reply = {http::status::forbidden, "application/json",
                 ErrorBody(grpc::StatusCode::PERMISSION_DENIED,
                           "WebSocket origin is not allowed", request_id)};
      } else if (monitor) {
        streamed = true;
        stream->expires_never();
        MonitorWebSocket(request, stream, request_id);
      } else if (request.method() == http::verb::options) {
        reply = {http::status::no_content, "application/json", {}};
      } else if (sse) {
        streamed = true;
        stream->expires_never();
        Stream(request, target, stream, request_id);
      } else {
        reply = Route(request.method(), target, request.body(), request_id);
      }
      const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - started)
                                   .count();
      if (streamed) {
        eos::lib::LogEvent(Logger(), spdlog::level::info, "north_http_stream_closed",
                           {{"method", std::string(request.method_string())},
                            {"path", target.path},
                            {"duration_ms", duration_ms}});
        break;
      }
      http::response<http::string_body> response(reply.status, request.version());
      response.set(http::field::content_type, reply.content_type);
      if (reply.no_store) response.set(http::field::cache_control, "no-store");
      ApplyCommonHeaders(&response, request, request_id);
      if (!reply.sample_age_ms.empty()) response.set("Eos-Sample-Age-Ms", reply.sample_age_ms);
      response.keep_alive(request.keep_alive());
      response.body() = std::move(reply.body);
      response.prepare_payload();
      const int status = response.result_int();
      const std::size_t response_bytes = response.body().size();
      http::write(*stream, response, error);
      eos::lib::LogEvent(
          Logger(), status >= 400 ? spdlog::level::warn : spdlog::level::debug,
          status >= 400 ? "north_http_request_rejected" : "north_http_request_completed",
          {{"method", std::string(request.method_string())},
           {"path", target.path},
           {"status", status},
           {"request_bytes", request.body().size()},
           {"response_bytes", response_bytes},
           {"duration_ms", duration_ms}});
      if (error || !response.keep_alive()) break;
    }
  }

  void CloseConnection(const std::shared_ptr<beast::tcp_stream>& stream) noexcept {
    beast::error_code ignored;
    stream->socket().shutdown(tcp::socket::shutdown_both, ignored);
    stream->socket().close(ignored);
    try {
      std::lock_guard<std::mutex> lock(state_mutex);
      streams.erase(stream);
    } catch (...) {
    }
  }

  void RunWorker(const std::shared_ptr<beast::tcp_stream>& stream,
                 const std::shared_ptr<std::atomic_bool>& done) noexcept {
    struct Completion {
      std::shared_ptr<std::atomic_bool> done;
      ~Completion() { done->store(true, std::memory_order_release); }
    } completion{done};
    try {
      Handle(stream);
    } catch (const std::exception& exception) {
      try {
        eos::lib::LogEvent(Logger(), spdlog::level::err, "north_http_worker_error",
                           {{"error", exception.what()}});
      } catch (...) {
      }
    } catch (...) {
      try {
        eos::lib::LogEvent(Logger(), spdlog::level::err, "north_http_worker_error",
                           {{"error", "unknown exception"}});
      } catch (...) {
      }
    }
    CloseConnection(stream);
  }

  void Shutdown() {
    if (stopping.exchange(true)) return;
    beast::error_code error;
    acceptor.close(error);
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      for (grpc::ClientContext* context : contexts) context->TryCancel();
      for (const auto& stream : streams) {
        stream->socket().cancel(error);
        stream->socket().close(error);
      }
    }
    for (auto& worker : workers)
      if (worker.thread.joinable()) worker.thread.join();
    workers.clear();
    is_listening.store(false);
  }
};

Server::Server(Config config, Client& client, audit::Service* audit)
    : impl_(new Impl(std::move(config), client, audit)) {}

Server::~Server() {
  impl_->Shutdown();
  delete impl_;
}

void Server::Run(const std::function<bool()>& stop_requested) {
  beast::error_code error;
  const auto address = asio::ip::make_address(impl_->config.host, error);
  if (error) throw std::system_error(error, "invalid North HTTP address");
  const tcp::endpoint endpoint(address, impl_->config.port);
  impl_->acceptor.open(endpoint.protocol(), error);
  if (!error) impl_->acceptor.set_option(asio::socket_base::reuse_address(true), error);
  if (!error) impl_->acceptor.bind(endpoint, error);
  const int listen_backlog = static_cast<int>(std::min<std::size_t>(
      impl_->config.max_connections, static_cast<std::size_t>(std::numeric_limits<int>::max())));
  if (!error) impl_->acceptor.listen(listen_backlog, error);
  if (error) throw std::system_error(error, "North HTTP bind failed");
  impl_->acceptor.non_blocking(true, error);
  if (error) throw std::system_error(error, "cannot configure North HTTP listener");
  impl_->is_listening.store(true);
  impl_->config.port = impl_->acceptor.local_endpoint().port();
  eos::lib::LogEvent(Logger(), spdlog::level::info, "north_http_listener_started",
                     {{"host", impl_->config.host},
                      {"port", impl_->config.port},
                      {"upstream", impl_->config.upstream}});
  while (!stop_requested() && !impl_->stopping.load()) {
    impl_->ReapWorkers();
    if (impl_->workers.size() >= impl_->config.max_connections) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }
    tcp::socket socket(impl_->io);
    impl_->acceptor.accept(socket, error);
    if (!error) {
      auto stream = std::make_shared<beast::tcp_stream>(std::move(socket));
      auto done = std::make_shared<std::atomic_bool>(false);
      {
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        impl_->streams.insert(stream);
      }
      try {
        impl_->workers.push_back(
            {std::thread([impl = impl_, stream, done] { impl->RunWorker(stream, done); }),
             std::move(done)});
      } catch (...) {
        impl_->CloseConnection(stream);
        impl_->Shutdown();
        throw;
      }
    } else if (error == asio::error::would_block || error == asio::error::try_again) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } else if (error != asio::error::operation_aborted) {
      impl_->Shutdown();
      throw std::system_error(error, "North HTTP accept failed");
    }
  }
  impl_->Shutdown();
  eos::lib::LogEvent(Logger(), spdlog::level::info, "north_http_listener_stopped");
}

bool Server::listening() const { return impl_->is_listening.load(); }

std::uint16_t Server::port() const { return impl_->config.port; }

}  // namespace eos::ext::north_http

/// HTTP/SSE client session for the standalone EOS adapter.
#include "session.h"

#include <algorithm>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

#include "src/eos/lib/log.h"
#include "src/eos/lib/uuid.h"

namespace eos::ext::repl {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using json = nlohmann::json;

constexpr std::size_t kMaxWatches = 16;
constexpr std::size_t kMaxSseEventBytes = 8U * 1024U * 1024U;

std::string UrlEncode(const std::string& value) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string result;
  for (unsigned char c : value) {
    if (std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '~') {
      result.push_back(static_cast<char>(c));
    } else {
      result.push_back('%');
      result.push_back(kHex[c >> 4]);
      result.push_back(kHex[c & 0x0f]);
    }
  }
  return result;
}

std::string WithoutResumeParameter(const std::string& target) {
  const auto question = target.find('?');
  if (question == std::string::npos) return target;
  std::string result = target.substr(0, question);
  std::istringstream query(target.substr(question + 1));
  std::string parameter;
  bool first = true;
  while (std::getline(query, parameter, '&')) {
    const std::string name = parameter.substr(0, parameter.find('='));
    if (name == "afterSequence" || name == "afterRevision" ||
        name == "afterJournalSequence" || name == "afterMonitorSequence" || name == "cursor")
      continue;
    result += first ? "?" : "&";
    result += parameter;
    first = false;
  }
  return result;
}

struct RequestSpec {
  http::verb verb{http::verb::get};
  std::string target;
  json body;
};

RequestSpec MapRequest(const std::string& method, const json& params) {
  if (method == "Discover") return {http::verb::get, "/v1/eos/discover", nullptr};
  if (method == "GetStatus") return {http::verb::get, "/v1/eos/status", nullptr};
  if (method == "GetAuditStatus") return {http::verb::get, "/v1/eos/audit/status", nullptr};
  if (method == "ListResources") return {http::verb::get, "/v1/eos/resources", nullptr};
  if (method == "ReadResource")
    return {http::verb::get, "/v1/eos/resource?uri=" + UrlEncode(params.value("uri", "")), nullptr};
  if (method == "ListNavigationPoints")
    return {http::verb::get,
            "/v1/eos/robots/" + UrlEncode(params.value("robot_name", "")) +
                "/navigation/waypoints",
            nullptr};
  if (method == "GetNavigationPoint" || method == "DeleteNavigationPoint")
    return {method == "GetNavigationPoint" ? http::verb::get : http::verb::delete_,
            "/v1/eos/robots/" + UrlEncode(params.value("robot_name", "")) +
                "/navigation/waypoints/" + UrlEncode(params.value("name", "")),
            nullptr};
  if (method == "PutNavigationPoint") {
    json body = params;
    const std::string robot = body.value("robot_name", "");
    const std::string name = body.value("name", "");
    body.erase("robot_name");
    body.erase("name");
    return {http::verb::put,
            "/v1/eos/robots/" + UrlEncode(robot) + "/navigation/waypoints/" + UrlEncode(name),
            std::move(body)};
  }
  if (method == "ReloadNavigationPoints")
    return {http::verb::post, "/v1/eos/navigation/waypoints:reload", json::object()};
  if (method == "QueryResourceHistory")
    return {http::verb::post, "/v1/eos/resource:history", params};
  if (method == "CallPrimitive") return {http::verb::post, "/v1/eos/primitives:call", params};
  if (method == "CancelPrimitive") return {http::verb::post, "/v1/eos/motions:cancel", params};
  if (method == "CaptureFrames") return {http::verb::post, "/v1/eos/world/camera:capture", params};
  if (method == "ObserveObjects")
    return {http::verb::post, "/v1/eos/perception/objects:observe", params};
  if (method == "GetPerceptionRoles")
    return {http::verb::get, "/v1/eos/perception/roles", nullptr};
  if (method == "InferVad") return {http::verb::post, "/v1/eos/perception/vad:infer", params};
  if (method == "DetectHumans")
    return {http::verb::post, "/v1/eos/perception/humans:detect", params};
  if (method == "EstimateHumanPose")
    return {http::verb::post, "/v1/eos/perception/human-poses:estimate", params};
  if (method == "DetectFaces")
    return {http::verb::post, "/v1/eos/perception/faces:detect", params};
  if (method == "ListSkills") return {http::verb::get, "/v1/eos/skills", nullptr};
  if (method == "GetSkill") {
    const json reference = params.value("reference", json::object());
    return {http::verb::get,
            "/v1/eos/skills/" + UrlEncode(reference.value("name", "")) + "/versions/" +
                UrlEncode(reference.value("version", "")),
            nullptr};
  }
  if (method == "StartExecution") {
    const json reference = params.value("reference", json::object());
    json body = params;
    body.erase("reference");
    return {http::verb::post,
            "/v1/eos/skills/" + UrlEncode(reference.value("name", "")) + "/versions/" +
                UrlEncode(reference.value("version", "")) + "/executions",
            std::move(body)};
  }
  if (method == "ListExecutions" || method == "PsExecutions")
    return {http::verb::get, "/v1/eos/executions", nullptr};
  if (method == "GetExecution")
    return {http::verb::get, "/v1/eos/executions/" + UrlEncode(params.value("execution_id", "")),
             nullptr};
  if (method == "GetExecutionState")
    return {http::verb::get,
            "/v1/eos/executions/" + UrlEncode(params.value("execution_id", "")) + "/state",
            nullptr};
  if (method == "TerminateExecution")
    return {http::verb::post,
            "/v1/eos/executions/" + UrlEncode(params.value("execution_id", "")) + ":terminate",
            json::object()};
  if (method == "TerminateExecutions")
    return {http::verb::post, "/v1/eos/executions:terminate", params};
  if (method == "SignalExecution") {
    json body = params;
    const std::string id = body.value("execution_id", "");
    body.erase("execution_id");
    return {http::verb::post, "/v1/eos/executions/" + UrlEncode(id) + ":signal", std::move(body)};
  }
  if (method == "ListWorkflowTemplates")
    return {http::verb::get, "/v1/eos/workflow-templates", nullptr};
  if (method == "GetWorkflowTemplate")
    return {http::verb::get,
            "/v1/eos/workflow-templates/" + UrlEncode(params.value("name", "")), nullptr};
  if (method == "RunWorkflowTemplate") {
    json body = params;
    const std::string name = body.value("name", "");
    body.erase("name");
    if (body.contains("workflow_id")) {
      body["workflowId"] = body.at("workflow_id");
      body.erase("workflow_id");
    }
    return {http::verb::post,
            "/v1/eos/workflow-templates/" + UrlEncode(name) + ":run", std::move(body)};
  }
  if (method == "ApplyActionList") return {http::verb::post, "/v1/eos/workflows", params};
  if (method == "ListActionLists") return {http::verb::get, "/v1/eos/workflows", nullptr};
  if (method == "GetActionList")
    return {http::verb::get, "/v1/eos/workflows/" + UrlEncode(params.value("workflow_id", "")),
            nullptr};
  if (method == "GetRuntimeSnapshot") return {http::verb::get, "/v1/eos/runtime:snapshot", nullptr};
  return {http::verb::unknown, {}, nullptr};
}

json DecodeResponse(const http::response<http::string_body>& response) {
  if (response.body().empty()) return json::object();
  try {
    return json::parse(response.body());
  } catch (const json::parse_error&) {
    return {{"error", {{"code", 13}, {"message", "adapter returned malformed JSON"}}}};
  }
}

}  // namespace

struct Session::Impl {
  struct Watch {
    std::mutex mutex;
    std::condition_variable wakeup;
    std::shared_ptr<asio::io_context> io;
    std::shared_ptr<beast::tcp_stream> stream;
    std::atomic_bool stop{false};
    std::atomic_bool done{false};
    std::thread thread;
  };

  Endpoint endpoint;
  std::atomic_bool connected{false};
  std::mutex mutex;
  std::map<std::string, std::unique_ptr<Watch>> watches;

  json Request(const RequestSpec& spec, int timeout_ms) {
    const std::string request_id = "repl-" + eos::lib::MintUuidV7();
    eos::lib::CorrelationScope correlation(request_id);
    const auto failed = [&](const std::string& message, const char* phase) {
      eos::lib::LogEvent(eos::lib::GetLogger("eos-repl"), spdlog::level::warn,
                         "repl_request_failed",
                         {{"method", std::string(http::to_string(spec.verb))},
                          {"path", spec.target.substr(0, spec.target.find('?'))},
                          {"phase", phase}});
      return json{{"error", {{"code", 14}, {"message", message}, {"requestId", request_id}}}};
    };
    const auto started = std::chrono::steady_clock::now();
    asio::io_context io;
    tcp::resolver resolver(io);
    beast::tcp_stream stream(io);
    beast::error_code error;
    const auto endpoints = resolver.resolve(endpoint.host, std::to_string(endpoint.port), error);
    if (error) return failed(error.message(), "resolve");
    stream.expires_after(std::chrono::milliseconds(timeout_ms));
    stream.connect(endpoints, error);
    if (error) return failed(error.message(), "connect");
    http::request<http::string_body> request(spec.verb, spec.target, 11);
    request.set(http::field::host, endpoint.host);
    request.set(http::field::accept, "application/json");
    request.set(http::field::user_agent, "eos-repl");
    request.set("X-Request-ID", request_id);
    if (spec.verb == http::verb::post || spec.verb == http::verb::put) {
      request.set(http::field::content_type, "application/json");
      request.body() = spec.body.is_null() ? "{}" : spec.body.dump();
      request.prepare_payload();
    }
    http::write(stream, request, error);
    if (error) return failed(error.message(), "write");
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(stream, buffer, response, error);
    if (error) return failed(error.message(), "read");
    beast::error_code ignored;
    stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
    const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
    eos::lib::LogEvent(
        eos::lib::GetLogger("eos-repl"),
        response.result_int() >= 400 ? spdlog::level::warn : spdlog::level::debug,
        response.result_int() >= 400 ? "repl_request_failed" : "repl_request_completed",
        {{"method", std::string(http::to_string(spec.verb))},
         {"path", spec.target.substr(0, spec.target.find('?'))},
         {"status", response.result_int()},
         {"duration_ms", duration_ms}});
    json decoded = DecodeResponse(response);
    if (response.result_int() >= 400) {
      if (decoded.contains("error")) return decoded;
      return {{"error", std::move(decoded)}};
    }
    return decoded;
  }

  bool StartWatch(const std::string& key, const std::string& target, StreamHandler handler,
                  std::string* error_message) {
    std::lock_guard<std::mutex> session_lock(mutex);
    for (auto it = watches.begin(); it != watches.end();) {
      if (!it->second->done.load(std::memory_order_acquire)) {
        ++it;
        continue;
      }
      if (it->second->thread.joinable()) it->second->thread.join();
      it = watches.erase(it);
    }
    if (!connected) {
      if (error_message) *error_message = "not connected";
      return false;
    }
    if (watches.find(key) != watches.end()) {
      if (error_message) *error_message = "watch already active";
      return false;
    }
    if (watches.size() >= kMaxWatches) {
      if (error_message) *error_message = "watch limit reached";
      return false;
    }
    auto watch = std::make_unique<Watch>();
    Watch* watch_state = watch.get();
    const Endpoint watch_endpoint = endpoint;
    watches.emplace(key, std::move(watch));
    try {
      watch_state->thread =
          std::thread([watch_state, watch_endpoint, target, handler = std::move(handler)] {
        struct Completion {
          Watch* watch;
          ~Completion() { watch->done.store(true, std::memory_order_release); }
        } completion{watch_state};
        try {
          std::string last_event_id;
          int reconnect_ms = 100;
          while (!watch_state->stop.load()) {
            const std::string request_id = "repl-" + eos::lib::MintUuidV7();
            eos::lib::CorrelationScope correlation(request_id);
            auto io = std::make_shared<asio::io_context>();
            auto stream = std::make_shared<beast::tcp_stream>(*io);
            {
              std::lock_guard<std::mutex> lock(watch_state->mutex);
              watch_state->io = io;
              watch_state->stream = stream;
            }
            tcp::resolver resolver(*io);
            beast::error_code error;
            const auto endpoints =
                resolver.resolve(watch_endpoint.host, std::to_string(watch_endpoint.port), error);
            if (!error && !watch_state->stop.load()) {
              stream->expires_after(std::chrono::seconds(2));
              stream->connect(endpoints, error);
            }
            if (!error && !watch_state->stop.load()) {
              const std::string attempt_target =
                  last_event_id.empty() ? target : WithoutResumeParameter(target);
              http::request<http::empty_body> request(http::verb::get, attempt_target, 11);
              request.set(http::field::host, watch_endpoint.host);
              request.set(http::field::accept, "text/event-stream");
              request.set(http::field::user_agent, "eos-repl");
              request.set("X-Request-ID", request_id);
              if (!last_event_id.empty()) request.set("Last-Event-ID", last_event_id);
              http::write(*stream, request, error);
              beast::flat_buffer header_buffer;
              http::response_parser<http::empty_body> parser;
              parser.skip(true);
              if (!error) http::read_header(*stream, header_buffer, parser, error);
              if (!error && parser.get().result_int() >= 400) {
                handler("error", {{"code", parser.get().result_int()},
                                  {"message", "SSE request was rejected"}});
                watch_state->stop.store(true);
                error = asio::error::operation_aborted;
              }
              stream->expires_never();
              asio::streambuf buffer(kMaxSseEventBytes);
              if (!error && header_buffer.size() > kMaxSseEventBytes) {
                error = asio::error::message_size;
              } else if (!error && header_buffer.size() != 0) {
                const auto bytes = header_buffer.size();
                asio::buffer_copy(buffer.prepare(bytes), header_buffer.data());
                buffer.commit(bytes);
              }
              while (!error && !watch_state->stop.load()) {
                const auto buffered = buffer.data();
                if (std::search(asio::buffers_begin(buffered), asio::buffers_end(buffered),
                                "\n\n", "\n\n" + 2) == asio::buffers_end(buffered))
                  asio::read_until(stream->socket(), buffer, "\n\n", error);
                if (error) {
                  if (error == asio::error::not_found) {
                    watch_state->stop.store(true);
                    handler("error", {{"code", 8}, {"message", "SSE event exceeds 8 MiB"}});
                  }
                  break;
                }
                std::istream input(&buffer);
                std::string line;
                std::string event;
                std::string event_id;
                std::string data;
                while (std::getline(input, line)) {
                  if (!line.empty() && line.back() == '\r') line.pop_back();
                  if (line.empty()) break;
                  if (line.rfind("event: ", 0) == 0)
                    event = line.substr(7);
                  else if (line.rfind("id: ", 0) == 0)
                    event_id = line.substr(4);
                  else if (line.rfind("data: ", 0) == 0)
                    data += line.substr(6);
                }
                if (data.empty()) continue;
                json payload;
                try {
                  payload = json::parse(data);
                } catch (const json::parse_error&) {
                  event = "error";
                  payload = {{"code", 13}, {"message", "invalid SSE data"}};
                }
                const bool continue_watching = handler(event, payload);
                if (event == "error" || !continue_watching) {
                  watch_state->stop.store(true);
                } else if (!event_id.empty()) {
                  last_event_id = std::move(event_id);
                }
              }
            }
            {
              std::lock_guard<std::mutex> lock(watch_state->mutex);
              if (watch_state->stream == stream) {
                watch_state->stream.reset();
                watch_state->io.reset();
              }
            }
            if (watch_state->stop.load()) break;
            eos::lib::LogEvent(eos::lib::GetLogger("eos-repl"), spdlog::level::warn,
                               "repl_stream_reconnecting",
                               {{"path", target.substr(0, target.find('?'))},
                                {"delay_ms", reconnect_ms},
                                {"has_last_event_id", !last_event_id.empty()}});
            std::unique_lock<std::mutex> lock(watch_state->mutex);
            watch_state->wakeup.wait_for(lock, std::chrono::milliseconds(reconnect_ms),
                                         [&] { return watch_state->stop.load(); });
            reconnect_ms = std::min(reconnect_ms * 2, 2000);
          }
        } catch (const std::exception& error) {
          watch_state->stop.store(true);
          try {
            eos::lib::LogEvent(eos::lib::GetLogger("eos-repl"), spdlog::level::err,
                               "repl_stream_failed", {{"reason", error.what()}});
          } catch (...) {
          }
        } catch (...) {
          watch_state->stop.store(true);
          try {
            eos::lib::LogEvent(eos::lib::GetLogger("eos-repl"), spdlog::level::err,
                               "repl_stream_failed", {{"reason", "unknown exception"}});
          } catch (...) {
          }
        }
        beast::error_code ignored;
        std::lock_guard<std::mutex> lock(watch_state->mutex);
        if (watch_state->stream) {
          watch_state->stream->socket().shutdown(tcp::socket::shutdown_both, ignored);
          watch_state->stream->socket().cancel(ignored);
          watch_state->stream->socket().close(ignored);
          watch_state->stream.reset();
          watch_state->io.reset();
        }
          });
    } catch (...) {
      watches.erase(key);
      if (error_message) *error_message = "unable to start watch";
      return false;
    }
    return true;
  }
};

Session::Session() : impl_(new Impl) {}

Session::~Session() {
  Close();
  delete impl_;
}

bool Session::IsConnected() const { return impl_->connected.load(); }

bool Session::Connect(const Endpoint& endpoint, int timeout_ms, std::string* error) {
  Close();
  impl_->endpoint = endpoint;
  const json result = impl_->Request({http::verb::get, "/health/live", nullptr}, timeout_ms);
  if (result.contains("error")) {
    if (error) *error = result.at("error").value("message", "adapter unavailable");
    return false;
  }
  impl_->connected = true;
  eos::lib::LogEvent(eos::lib::GetLogger("eos-repl"), spdlog::level::info, "repl_connected",
                     {{"host", endpoint.host}, {"port", endpoint.port}, {"transport", "http"}});
  return true;
}

void Session::Close() {
  std::map<std::string, std::unique_ptr<Impl::Watch>> watches;
  bool was_connected = false;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    was_connected = impl_->connected.exchange(false);
    watches = std::move(impl_->watches);
  }
  for (auto& [key, watch] : watches) {
    watch->stop.store(true);
    watch->wakeup.notify_all();
    beast::error_code ignored;
    std::lock_guard<std::mutex> lock(watch->mutex);
    if (watch->stream) {
      watch->stream->socket().shutdown(tcp::socket::shutdown_both, ignored);
      watch->stream->socket().cancel(ignored);
      watch->stream->socket().close(ignored);
    }
  }
  for (auto& [key, watch] : watches)
    if (watch->thread.joinable()) watch->thread.join();
  if (was_connected)
    eos::lib::LogEvent(eos::lib::GetLogger("eos-repl"), spdlog::level::info, "repl_disconnected",
                       {{"watch_count", watches.size()}});
}

json Session::Call(const std::string& method, const json& params, int timeout_ms) {
  if (!impl_->connected) return {{"error", {{"code", 14}, {"message", "not connected"}}}};
  const RequestSpec spec = MapRequest(method, params.is_null() ? json::object() : params);
  if (spec.verb == http::verb::unknown)
    return {{"error", {{"code", 12}, {"message", "unknown RPC '" + method + "'"}}}};
  return impl_->Request(spec, timeout_ms);
}

bool Session::Watch(const std::string& uri, UpdateHandler handler, std::string* error) {
  Unwatch(uri);
  return impl_->StartWatch(
      "resource:" + uri, "/v1/eos/resource:watch?uri=" + UrlEncode(uri),
      [handler = std::move(handler)](const std::string& event, const json& value) {
        if (event == "resource-changed" && value.contains("resource"))
          handler(value.at("resource"));
        return true;
      },
      error);
}

bool Session::WatchStream(const std::string& key, const std::string& path, StreamHandler handler,
                          std::string* error) {
  Unwatch(key);
  return impl_->StartWatch(key, path, std::move(handler), error);
}

void Session::Unwatch(const std::string& key) {
  std::unique_ptr<Impl::Watch> watch;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto found = impl_->watches.find(key);
    if (found == impl_->watches.end()) {
      found = impl_->watches.find("resource:" + key);
      if (found == impl_->watches.end()) return;
    }
    watch = std::move(found->second);
    impl_->watches.erase(found);
  }
  beast::error_code ignored;
  watch->stop.store(true);
  watch->wakeup.notify_all();
  {
    std::lock_guard<std::mutex> lock(watch->mutex);
    if (watch->stream) {
      watch->stream->socket().shutdown(tcp::socket::shutdown_both, ignored);
      watch->stream->socket().cancel(ignored);
      watch->stream->socket().close(ignored);
    }
  }
  if (watch->thread.joinable()) watch->thread.join();
}

}  // namespace eos::ext::repl

/// Standalone HTTP/SSE adapter client session for eos-repl.
#pragma once

#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>

namespace eos::ext::repl {

class Session final {
 public:
  using UpdateHandler = std::function<void(const nlohmann::json& resource)>;
  using StreamHandler =
      std::function<bool(const std::string& event, const nlohmann::json& payload)>;

  struct Endpoint {
    std::string host = "127.0.0.1";
    std::uint16_t port = 16005;
  };

  Session();
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  bool IsConnected() const;
  bool Connect(const Endpoint& endpoint, int timeout_ms, std::string* error);
  void Close();

  /// Calls a named GatewayService RPC and returns its payload or an error object.
  nlohmann::json Call(const std::string& method, const nlohmann::json& params, int timeout_ms);

  /// Starts/cancels one WatchResource stream per URI.
  bool Watch(const std::string& uri, UpdateHandler handler, std::string* error);
  /// Starts an arbitrary adapter SSE stream under a caller-owned key.
  bool WatchStream(const std::string& key, const std::string& path, StreamHandler handler,
                   std::string* error);
  void Unwatch(const std::string& uri);

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace eos::ext::repl

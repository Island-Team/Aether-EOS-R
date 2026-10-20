/// Blocking protocol clients used by the camera workers for streaming vision models.
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "src/eos/runtime/models/runtime.h"

namespace eos::runtime::models {

class StreamCancelled final : public std::runtime_error {
 public:
  StreamCancelled() : std::runtime_error("WebSocket operation cancelled") {}
};

class StreamTimedOut final : public std::runtime_error {
 public:
  StreamTimedOut() : std::runtime_error("WebSocket operation timed out") {}
};

class StreamProtocolError final : public std::runtime_error {
 public:
  explicit StreamProtocolError(std::string message) : std::runtime_error(std::move(message)) {}
};

/// Reusable bounded WebSocket transport. Model-specific framing and response
/// semantics belong to external adapters.
class WebSocketClient final {
 public:
  using Deadline = std::chrono::steady_clock::time_point;

  explicit WebSocketClient(const ModelConfig& config);
  ~WebSocketClient();
  WebSocketClient(const WebSocketClient&) = delete;
  WebSocketClient& operator=(const WebSocketClient&) = delete;

  void Connect(const CancelRequested& cancel_requested = {}, Deadline deadline = Deadline::max());
  void WriteText(std::string_view value, const CancelRequested& cancel_requested = {},
                 Deadline deadline = Deadline::max());
  void WriteBinary(const std::vector<std::uint8_t>& value,
                   const CancelRequested& cancel_requested = {},
                   Deadline deadline = Deadline::max());
  nlohmann::json ReadJson(const CancelRequested& cancel_requested = {},
                          Deadline deadline = Deadline::max());
  void Close() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace eos::runtime::models

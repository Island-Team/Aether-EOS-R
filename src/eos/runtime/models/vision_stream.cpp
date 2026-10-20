#include "src/eos/runtime/models/vision_stream.h"

#include <algorithm>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace eos::runtime::models {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using Deadline = std::chrono::steady_clock::time_point;

struct Endpoint {
  std::string host;
  std::string authority;
  std::string target;
  std::uint16_t port{0};
};

Endpoint ParseWebSocketEndpoint(const std::string& value) {
  constexpr std::string_view kScheme = "ws://";
  if (value.rfind(kScheme, 0) != 0) {
    throw std::invalid_argument("WebSocket endpoint must use ws://");
  }
  const std::string remainder = value.substr(kScheme.size());
  const std::size_t target_start = remainder.find_first_of("/?");
  const std::string authority = remainder.substr(0, target_start);
  if (authority.empty() || authority.find('@') != std::string::npos ||
      authority.find_first_of(" \t\r\n\\#") != std::string::npos) {
    throw std::invalid_argument("invalid WebSocket endpoint authority");
  }

  Endpoint endpoint;
  endpoint.authority = authority;
  endpoint.target = target_start == std::string::npos ? "/" : remainder.substr(target_start);
  if (!endpoint.target.empty() && endpoint.target.front() == '?') {
    endpoint.target.insert(endpoint.target.begin(), '/');
  }
  std::string port_text = "80";
  if (authority.front() == '[') {
    const std::size_t close = authority.find(']');
    if (close == std::string::npos || close == 1) {
      throw std::invalid_argument("invalid WebSocket IPv6 host");
    }
    endpoint.host = authority.substr(1, close - 1);
    if (close + 1 < authority.size()) {
      if (authority[close + 1] != ':' || close + 2 == authority.size()) {
        throw std::invalid_argument("invalid WebSocket port");
      }
      port_text = authority.substr(close + 2);
    }
  } else {
    const std::size_t colon = authority.rfind(':');
    if (colon == std::string::npos) {
      endpoint.host = authority;
    } else {
      endpoint.host = authority.substr(0, colon);
      port_text = authority.substr(colon + 1);
    }
  }
  if (endpoint.host.empty() || port_text.empty() ||
      !std::all_of(port_text.begin(), port_text.end(),
                   [](unsigned char value) { return std::isdigit(value) != 0; })) {
    throw std::invalid_argument("invalid WebSocket host or port");
  }
  const unsigned long parsed_port = std::stoul(port_text);
  if (parsed_port == 0 || parsed_port > 65535) {
    throw std::invalid_argument("WebSocket port must be in (0, 65535]");
  }
  beast::error_code address_error;
  asio::ip::make_address(endpoint.host, address_error);
  if (address_error) {
    throw std::invalid_argument("WebSocket host must be a numeric IP address");
  }
  endpoint.port = static_cast<std::uint16_t>(parsed_port);
  return endpoint;
}

class BlockingWebSocket final {
 public:
  BlockingWebSocket(std::string endpoint, std::chrono::milliseconds timeout,
                    std::size_t max_response_bytes)
      : endpoint_(ParseWebSocketEndpoint(endpoint)),
        timeout_(timeout),
        max_response_bytes_(max_response_bytes) {}

  ~BlockingWebSocket() { Close(); }

  void Connect(const CancelRequested& cancel_requested = {}, Deadline deadline = Deadline::max()) {
    // Beast can expire an idle connection while no io_context work is running.
    // Reusing that socket fails the next write immediately, before inference starts.
    if (open_ && std::chrono::steady_clock::now() - last_activity_ >= timeout_ / 2) Abort();
    if (open_) return;
    Close();
    stream_ = std::make_unique<websocket::stream<beast::tcp_stream>>(io_);
    const auto address = asio::ip::make_address(endpoint_.host);
    beast::get_lowest_layer(*stream_).expires_after(timeout_);
    beast::error_code error;
    Begin(cancel_requested, deadline);
    beast::get_lowest_layer(*stream_).async_connect(tcp::endpoint(address, endpoint_.port),
                                                    [this, &error](beast::error_code value) {
                                                      error = value;
                                                      Finish();
                                                    });
    Run();
    if (error) throw beast::system_error(error);
    beast::get_lowest_layer(*stream_).expires_never();
    websocket::stream_base::timeout options =
        websocket::stream_base::timeout::suggested(beast::role_type::client);
    options.handshake_timeout = timeout_;
    options.idle_timeout = timeout_;
    stream_->set_option(options);
    stream_->read_message_max(max_response_bytes_);
    Begin(cancel_requested, deadline);
    stream_->async_handshake(endpoint_.authority, endpoint_.target,
                             [this, &error](beast::error_code value) {
                               error = value;
                               Finish();
                             });
    Run();
    if (error) throw beast::system_error(error);
    open_ = true;
    last_activity_ = std::chrono::steady_clock::now();
  }

  void WriteText(std::string_view value, const CancelRequested& cancel_requested = {},
                 Deadline deadline = Deadline::max()) {
    Connect(cancel_requested, deadline);
    stream_->text(true);
    beast::error_code error;
    Begin(cancel_requested, deadline);
    stream_->async_write(asio::buffer(value), [this, &error](beast::error_code value, std::size_t) {
      error = value;
      Finish();
    });
    Run();
    if (error) throw beast::system_error(error);
    last_activity_ = std::chrono::steady_clock::now();
  }

  void WriteBinary(const std::vector<std::uint8_t>& value,
                   const CancelRequested& cancel_requested = {},
                   Deadline deadline = Deadline::max()) {
    Connect(cancel_requested, deadline);
    stream_->binary(true);
    beast::error_code error;
    Begin(cancel_requested, deadline);
    stream_->async_write(asio::buffer(value),
                         [this, &error](beast::error_code result, std::size_t) {
                           error = result;
                           Finish();
                         });
    Run();
    if (error) throw beast::system_error(error);
    last_activity_ = std::chrono::steady_clock::now();
  }

  nlohmann::json ReadJson(const CancelRequested& cancel_requested = {},
                          Deadline deadline = Deadline::max()) {
    beast::flat_buffer buffer;
    beast::error_code error;
    Begin(cancel_requested, deadline);
    stream_->async_read(buffer, [this, &error](beast::error_code value, std::size_t) {
      error = value;
      Finish();
    });
    Run();
    if (error) throw beast::system_error(error);
    last_activity_ = std::chrono::steady_clock::now();
    if (!stream_->got_text()) throw std::runtime_error("expected a text WebSocket message");
    return nlohmann::json::parse(beast::buffers_to_string(buffer.data()));
  }

  void Close() noexcept {
    if (open_) {
      try {
        beast::error_code error;
        Begin({}, std::chrono::steady_clock::now() + timeout_);
        stream_->async_close(websocket::close_code::normal,
                             [this, &error](beast::error_code value) {
                               error = value;
                               Finish();
                             });
        Run();
      } catch (...) {
      }
    }
    Abort();
  }

 private:
  void Abort() noexcept {
    if (stream_) {
      beast::error_code ignored;
      beast::get_lowest_layer(*stream_).socket().close(ignored);
    }
    stream_.reset();
    open_ = false;
  }
  void Begin(CancelRequested cancel_requested, Deadline deadline) {
    cancel_requested_ = std::move(cancel_requested);
    cancel_detected_ = false;
    timeout_detected_ = false;
    operation_done_ = false;
    cancel_timer_ = std::make_unique<asio::steady_timer>(io_);
    deadline_timer_ = std::make_unique<asio::steady_timer>(io_);
    if (deadline == Deadline::max()) deadline = std::chrono::steady_clock::now() + timeout_;
    deadline_timer_->expires_at(deadline);
    deadline_timer_->async_wait([this](beast::error_code error) {
      if (error == asio::error::operation_aborted) return;
      timeout_detected_ = true;
      beast::error_code ignored;
      if (stream_) beast::get_lowest_layer(*stream_).socket().close(ignored);
    });
    PollCancellation();
  }

  void PollCancellation() {
    if (!cancel_requested_) return;
    cancel_timer_->expires_after(std::chrono::milliseconds(10));
    cancel_timer_->async_wait([this](beast::error_code error) {
      if (error == asio::error::operation_aborted) return;
      if (cancel_requested_ && cancel_requested_()) {
        cancel_detected_ = true;
        beast::error_code ignored;
        if (stream_) beast::get_lowest_layer(*stream_).socket().close(ignored);
        return;
      }
      PollCancellation();
    });
  }

  void Finish() {
    operation_done_ = true;
    if (cancel_timer_) {
      cancel_timer_->cancel();
    }
    if (deadline_timer_) {
      deadline_timer_->cancel();
    }
  }

  void Run() {
    io_.restart();
    while (!operation_done_ && io_.run_one() != 0) {
    }
    cancel_timer_.reset();
    deadline_timer_.reset();
    cancel_requested_ = {};
    if (cancel_detected_) throw StreamCancelled();
    if (timeout_detected_) throw StreamTimedOut();
  }

  Endpoint endpoint_;
  std::chrono::milliseconds timeout_;
  std::size_t max_response_bytes_;
  asio::io_context io_;
  std::unique_ptr<websocket::stream<beast::tcp_stream>> stream_;
  std::unique_ptr<asio::steady_timer> cancel_timer_;
  std::unique_ptr<asio::steady_timer> deadline_timer_;
  CancelRequested cancel_requested_;
  bool cancel_detected_{false};
  bool timeout_detected_{false};
  bool operation_done_{false};
  bool open_{false};
  Deadline last_activity_{};
};

}  // namespace

struct WebSocketClient::Impl {
  explicit Impl(const ModelConfig& config)
      : stream(config.endpoint, config.timeout, config.max_response_bytes) {}
  BlockingWebSocket stream;
};

WebSocketClient::WebSocketClient(const ModelConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}
WebSocketClient::~WebSocketClient() = default;
void WebSocketClient::Connect(const CancelRequested& cancel_requested, Deadline deadline) {
  impl_->stream.Connect(cancel_requested, deadline);
}
void WebSocketClient::WriteText(std::string_view value, const CancelRequested& cancel_requested,
                                Deadline deadline) {
  impl_->stream.WriteText(value, cancel_requested, deadline);
}
void WebSocketClient::WriteBinary(const std::vector<std::uint8_t>& value,
                                  const CancelRequested& cancel_requested, Deadline deadline) {
  impl_->stream.WriteBinary(value, cancel_requested, deadline);
}
nlohmann::json WebSocketClient::ReadJson(const CancelRequested& cancel_requested,
                                         Deadline deadline) {
  try {
    return impl_->stream.ReadJson(cancel_requested, deadline);
  } catch (const beast::system_error& error) {
    if (error.code() == websocket::error::message_too_big) {
      throw StreamProtocolError("WebSocket response exceeds configured byte limit");
    }
    throw;
  }
}
void WebSocketClient::Close() noexcept { impl_->stream.Close(); }

}  // namespace eos::runtime::models

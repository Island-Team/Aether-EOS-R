/// Standalone HTTP/ProtoJSON and SSE front end for the EOS gRPC services.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>

#include "src/eos/ext/north_http/config/config.h"

namespace eos::ext::north_http {

class Client;
namespace audit {
class Service;
}

/// Blocking north HTTP server. Each accepted HTTP connection is isolated from EOS
/// and reaches the core exclusively through generated gRPC client stubs.
class Server final {
 public:
  /// Creates a server using the process-owned shared EOS gRPC client.
  Server(Config config, Client& client, audit::Service* audit = nullptr);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  /// Serves until stop_requested returns true. Throws when the listener cannot bind.
  void Run(const std::function<bool()>& stop_requested);

  /// True after the HTTP listener has bound successfully.
  bool listening() const;

  /// Bound listener port, including the selected ephemeral port after binding port zero.
  std::uint16_t port() const;

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace eos::ext::north_http

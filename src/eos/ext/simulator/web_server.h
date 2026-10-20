#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "src/eos/ext/simulator/map_store.h"

namespace eos::ext::simulator {
class World;

struct WebResponse {
  unsigned status{200};
  std::string content_type{"application/json"};
  std::string body;
  std::string etag;
};

class WebServer {
 public:
  WebServer(MapStore maps, SourceLibrary sources, World& world, std::string active_map,
            std::function<void()> reload);
  WebResponse Handle(const std::string& method, const std::string& target,
                     const std::string& body = {}, const std::string& content_type = {},
                     const std::string& if_match = {});
  void Serve(const std::string& host, std::uint16_t port, const std::atomic_bool& stop);

 private:
  MapStore maps_;
  SourceLibrary sources_;
  World& world_;
  std::string active_map_;
  std::function<void()> reload_;
  std::atomic_uint64_t generation_{0};
};

bool SimulatorCorsOriginAllowed(const std::string& origin);
}  // namespace eos::ext::simulator

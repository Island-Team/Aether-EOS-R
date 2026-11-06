/// Side-effect-free IsOS configuration boundary for EOS.
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

#include "adapters/eos-cxx/runtime/init/config.h"

namespace {
using Json = nlohmann::json;

Json Envelope(bool ok, Json data = Json::object(), Json diagnostics = Json::array()) {
  return {{"schema_version", 1},
          {"ok", ok},
          {"data", std::move(data)},
          {"diagnostics", std::move(diagnostics)}};
}

Json Contracts() {
  return Json::array({
      {{"name", "island.eos.v1.GatewayService"}, {"version", "1.0.0"}},
      {{"name", "island.eos.v1.SkillService"}, {"version", "1.0.0"}},
      {{"name", "island.eos.v2.CapabilityService"}, {"version", "2.0.0"}},
      {{"name", "island.eos.v1.WorkflowService"}, {"version", "1.0.0"}},
  });
}

Json Defaults() {
  return {{"host", "127.0.0.1"},           {"grpc_port", 16200},
          {"grpc_uds_path", ""},           {"grpc_uds_mode", 384},
          {"north_http_port", 16201},      {"binding_backend", "bindings/isos-demo.toml"},
          {"binding_profile", "isos-demo"}};
}

std::string StarterBinding() {
  // Self-contained source and installed artifact: IsOS writes this beside eos.toml.
  // The demo backend requires a separate simulator IPC process for ready devices.
  return R"toml([profile.isos-demo.robot]
robot_id = "isos-demo-0"
robot_name = "isos-demo-0"
model = "eos-demo"
robot_desc = "IsOS starter robot; demo operations require a simulator service."

[[profile.isos-demo.device]]
class = "speaker"
part_id = "default"
description = "demo speaker"

[[profile.isos-demo.device.primitive]]
name = "speaker.play"
version = "1.0.0"
description = "Play audio through the demo simulator."

[profile.isos-demo.device.backend]
type = "demo"

[profile.isos-demo.device.feeds.proprioception]
max_rate_hz = 10.0

[profile.isos-demo.device.feeds.health]
max_rate_hz = 10.0

[[profile.isos-demo.device]]
class = "base"
part_id = "default"
description = "demo base"

[[profile.isos-demo.device.primitive]]
name = "base.posture"
version = "1.0.0"
description = "Set demo base posture."

[profile.isos-demo.device.backend]
type = "demo"

[profile.isos-demo.device.feeds.health]
max_rate_hz = 10.0
)toml";
}

Json Fields() {
  return Json::array({
      {{"name", "host"},
       {"type", "string"},
       {"label", "Listener host"},
       {"description", "Loopback address for EOS and north HTTP listeners."},
       {"default", "127.0.0.1"},
       {"options", Json::array({"127.0.0.1", "::1", "localhost"})}},
      {{"name", "grpc_port"},
       {"type", "integer"},
       {"label", "gRPC port"},
       {"description", "TCP port for the EOS gRPC gateway."},
       {"default", 16200},
       {"min", 1},
       {"max", 65535}},
      {{"name", "grpc_uds_path"},
       {"type", "absolute_path"},
       {"label", "gRPC socket path"},
       {"description", "Derived from the eos.uds endpoint socket path when managed by IsOS."},
       {"default", ""}},
      {{"name", "grpc_uds_mode"},
       {"type", "integer"},
       {"label", "gRPC socket mode"},
       {"description", "Unix socket permission bits as a decimal integer."},
       {"default", 384},
       {"min", 1},
       {"max", 511}},
      {{"name", "north_http_port"},
       {"type", "integer"},
       {"label", "North HTTP port"},
       {"description", "TCP port for the north HTTP gateway; must differ from the gRPC port."},
       {"default", 16201},
       {"min", 1},
       {"max", 65535}},
      {{"name", "binding_backend"},
       {"type", "string"},
       {"label", "Binding backend"},
       {"description", "Path to the hardware binding file relative to eos/eos.toml."},
       {"default", "bindings/isos-demo.toml"},
       {"required", true}},
      {{"name", "binding_profile"},
       {"type", "string"},
       {"label", "Binding profile"},
       {"description", "Profile name in the selected hardware binding file."},
       {"default", "isos-demo"},
       {"required", true}},
  });
}

Json Profiles() {
  const auto contracts = Contracts();
  return Json::array({
      {{"id", "eos.native"},
       {"version", "1.0.0"},
       {"template", "process"},
       {"inputs", Json::array()},
       {"outputs", Json::array({{{"name", "grpc"},
                                 {"type", "grpc"},
                                 {"transport", "tcp"},
                                 {"contracts", contracts}}})},
       {"fields", Fields()},
       {"provides", contracts},
       {"requires", Json::array()},
       {"readiness", {{"kind", "tcp"}, {"field", "grpc_port"}}}},
      {{"id", "eos.uds"},
       {"version", "1.0.0"},
       {"template", "endpoint"},
       {"parent_profile", "eos.native"},
       {"inputs", Json::array()},
       {"outputs", Json::array({{{"name", "grpc"},
                                 {"type", "grpc"},
                                 {"transport", "uds"},
                                 {"contracts", contracts}}})},
       {"fields",
        Json::array(
            {{{"name", "socket_path"},
              {"type", "absolute_path"},
              {"label", "Socket path"},
              {"description", "Private absolute Unix socket path owned by the parent EOS process."},
              {"required", true}}})},
       {"provides", contracts},
       {"requires", Json::array()},
       {"readiness", {{"kind", "uds"}, {"field", "socket_path"}}}},
  });
}

std::string StringValue(const Json& values, const char* key) {
  if (!values.at(key).is_string()) throw std::runtime_error(std::string(key) + " must be a string");
  return values.at(key).get<std::string>();
}

int PortValue(const Json& values, const char* key) {
  if (!values.at(key).is_number_integer())
    throw std::runtime_error(std::string(key) + " must be an integer");
  const auto value = values.at(key).get<int>();
  if (value < 1 || value > 65535)
    throw std::runtime_error(std::string(key) + " must be in [1, 65535]");
  return value;
}

std::string Quote(const std::string& value) { return Json(value).dump(); }

Json Render(const Json& values) {
  if (!values.is_object()) throw std::runtime_error("values must be an object");
  Json resolved = Defaults();
  if (values.contains("socket_path") && values.contains("grpc_uds_path") &&
      values.at("socket_path") != values.at("grpc_uds_path"))
    throw std::runtime_error("socket_path and grpc_uds_path must match");
  for (auto it = values.begin(); it != values.end(); ++it) {
    if (it.key() == "socket_path") {
      resolved["grpc_uds_path"] = it.value();
      continue;
    }
    if (!resolved.contains(it.key())) throw std::runtime_error("unknown field: " + it.key());
    resolved[it.key()] = it.value();
  }
  const auto host = StringValue(resolved, "host");
  const auto uds = StringValue(resolved, "grpc_uds_path");
  const auto backend = StringValue(resolved, "binding_backend");
  const auto profile = StringValue(resolved, "binding_profile");
  const int grpc_port = PortValue(resolved, "grpc_port");
  const int http_port = PortValue(resolved, "north_http_port");
  if (host != "127.0.0.1" && host != "::1" && host != "localhost")
    throw std::runtime_error("host must be loopback");
  if (grpc_port == http_port) throw std::runtime_error("grpc_port and north_http_port must differ");
  if (backend.empty() || profile.empty())
    throw std::runtime_error("binding_backend and binding_profile must be non-empty");
  if (!resolved.at("grpc_uds_mode").is_number_integer())
    throw std::runtime_error("grpc_uds_mode must be an integer");
  const int uds_mode = resolved.at("grpc_uds_mode").get<int>();
  if (uds_mode < 1 || uds_mode > 0777)
    throw std::runtime_error("grpc_uds_mode must be in [1, 511]");
  std::string eos =
      "[gateway]\nhost = " + Quote(host) + "\ngrpc_port = " + std::to_string(grpc_port) + "\n";
  if (!uds.empty()) {
    eos += "grpc_uds_path = " + Quote(uds) + "\ngrpc_uds_mode = 0o";
    char mode[5]{};
    std::snprintf(mode, sizeof(mode), "%03o", uds_mode);
    eos += mode;
    eos += "\n";
  }
  eos += "\n[mode]\n[[mode.binding]]\nbackend = " + Quote(backend) +
         "\nprofile = " + Quote(profile) + "\n";
  // Reuse the production parser, including mode and perception validation.
  (void)eos::runtime::init::ParseConfig(eos);
  const std::string upstream = uds.empty() ? host + ":" + std::to_string(grpc_port) : "unix:" + uds;
  const std::string http = "[north_http]\nhost = " + Quote(host) +
                           "\nport = " + std::to_string(http_port) +
                           "\nupstream = " + Quote(upstream) + "\n";
  Json files = Json::array({{{"path", "eos/eos.toml"}, {"content", eos}},
                            {{"path", "ext/north-http/config.toml"}, {"content", http}}});
  if (backend == "bindings/isos-demo.toml" && profile == "isos-demo") {
    files.push_back({{"path", "eos/bindings/isos-demo.toml"}, {"content", StarterBinding()}});
  }
  return {{"files", std::move(files)}};
}

std::string ErrorPath(const std::string& message) {
  for (const char* field :
       {"socket_path", "grpc_uds_path", "grpc_uds_mode", "grpc_port", "north_http_port",
        "binding_backend", "binding_profile", "host", "values"}) {
    if (message.find(field) != std::string::npos) return std::string("values.") + field;
  }
  return "values";
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
      std::cout << "usage: eos-config <describe|defaults|profiles|render|validate> --json\n";
      return 0;
    }
    if (argc != 3 || std::string(argv[2]) != "--json")
      throw std::runtime_error(
          "usage: eos-config <describe|defaults|profiles|render|validate> --json");
    const std::string command(argv[1]);
    Json data;
    if (command == "describe") {
      data = {{"component", "eos"},
              {"version", EOS_PRODUCT_VERSION},
              {"provides", Contracts()},
              {"requires", Json::array()}};
    } else if (command == "defaults") {
      data = Defaults();
    } else if (command == "profiles") {
      data = {{"profiles", Profiles()}};
    } else if (command == "render" || command == "validate") {
      const std::string input(std::istreambuf_iterator<char>(std::cin), {});
      const Json request = Json::parse(input);
      if (!request.is_object() || request.value("schema_version", 0) != 1 ||
          !request.contains("values"))
        throw std::runtime_error("request requires schema_version 1 and values");
      const Json rendered = Render(request.at("values"));
      data = command == "render" ? rendered : Json{{"valid", true}};
    } else {
      throw std::runtime_error("unknown command: " + command);
    }
    std::cout << Envelope(true, data).dump() << '\n';
    return 0;
  } catch (const std::exception& error) {
    const std::string message(error.what());
    std::cout << Envelope(false, Json::object(),
                          Json::array({{{"path", ErrorPath(message)},
                                        {"code", "invalid_config"},
                                        {"message", message}}}))
                     .dump()
              << '\n';
    return 1;
  }
}

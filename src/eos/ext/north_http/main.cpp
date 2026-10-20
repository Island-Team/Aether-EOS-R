/// eos-north-http — standalone HTTP/SSE to EOS gRPC bridge.
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

#include "src/eos/ext/north_http/audit/audit.h"
#include "src/eos/ext/north_http/client/client.h"
#include "src/eos/ext/north_http/config/config.h"
#include "src/eos/ext/north_http/server/server.h"
#include "src/eos/lib/log.h"
#include "src/eos/lib/paths.h"

namespace {

std::atomic_bool g_stop{false};
void Signal(int) { g_stop.store(true); }

struct Options {
  std::string config;
  std::string etc;
  std::string contracts;
  std::string host;
  std::string upstream;
  std::string log_level;
  std::uint16_t port{0};
};

std::uint16_t ParsePort(const std::string& text) {
  unsigned int port = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), port);
  if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      port == 0 || port > 65535) {
    throw std::runtime_error("--port must be an integer in (0, 65535]");
  }
  return static_cast<std::uint16_t>(port);
}

Options ParseArgs(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= argc) throw std::runtime_error(arg + " requires a value");
      return argv[++i];
    };
    if (arg == "--config")
      options.config = value();
    else if (arg == "--etc")
      options.etc = value();
    else if (arg == "--contracts")
      options.contracts = value();
    else if (arg == "--host")
      options.host = value();
    else if (arg == "--port")
      options.port = ParsePort(value());
    else if (arg == "--upstream")
      options.upstream = value();
    else if (arg == "--log-level")
      options.log_level = value();
    else if (arg == "--help") {
      std::fprintf(stderr,
                   "usage: eos-north-http [--config PATH] [--etc PATH] "
                   "[--contracts PATH] [--host ADDR] [--port N] "
                   "[--upstream HOST:PORT] [--log-level LEVEL]\n");
      std::exit(0);
    } else {
      throw std::runtime_error("unknown option '" + arg + "'");
    }
  }
  return options;
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, Signal);
  std::signal(SIGTERM, Signal);
  try {
    const Options options = ParseArgs(argc, argv);
    const eos::lib::LayoutPaths paths =
        eos::lib::ResolveLayoutPaths(options.etc, options.contracts);
    const std::filesystem::path config_path = options.config.empty()
                                                  ? paths.etc / "north-http.toml"
                                                  : std::filesystem::path(options.config);
    if (!config_path.is_absolute()) throw std::runtime_error("config path must be absolute");
    eos::ext::north_http::Config config = eos::ext::north_http::LoadConfig(config_path.string());
    if (!options.host.empty()) config.host = options.host;
    if (options.port != 0) config.port = options.port;
    if (!options.upstream.empty()) config.upstream = options.upstream;
    eos::ext::north_http::ValidateUpstream(config.upstream);
    if (!config.allow_remote && !eos::ext::north_http::IsLoopbackHost(config.host)) {
      throw std::runtime_error("non-loopback host requires allow_remote = true in configuration");
    }
    if (!options.log_level.empty()) config.log.console_level = options.log_level;
    eos::lib::ConfigureLogging(paths.log, "north-http", config.log);
    auto logger = eos::lib::GetLogger("north-http");
    eos::lib::LogEvent(logger, spdlog::level::info, "north_http_process_started",
                       {{"upstream", config.upstream}});
    eos::ext::north_http::Client client(config.upstream);
    std::unique_ptr<eos::ext::north_http::audit::Service> audit;
    if (config.action_list_audit.enabled) {
      eos::ext::north_http::audit::Config audit_config;
      audit_config.directory = paths.state / "north-http" / "action-list-audit";
      audit_config.rotation_size_bytes = config.action_list_audit.rotation_size_bytes;
      audit_config.rotation_files = config.action_list_audit.rotation_files;
      audit_config.reconnect_delay =
          std::chrono::milliseconds(config.action_list_audit.reconnect_delay_ms);
      audit = std::make_unique<eos::ext::north_http::audit::Service>(client, audit_config);
      audit->Start();
    }
    eos::ext::north_http::Server server(std::move(config), client, audit.get());
    server.Run([] { return g_stop.load(); });
    if (audit) audit->Stop();
    eos::lib::LogEvent(logger, spdlog::level::info, "north_http_process_stopped");
    return 0;
  } catch (const std::exception& error) {
    try {
      eos::lib::ConfigureLogging(eos::lib::DefaultLogDirectory(), "north-http",
                                 eos::lib::LogConfig{});
      eos::lib::LogEvent(eos::lib::GetLogger("north-http"), spdlog::level::err,
                         "north_http_process_failed", {{"reason", "startup_exception"}});
    } catch (...) {
    }
    std::fprintf(stderr, "eos-north-http: %s\n", error.what());
    return 1;
  }
}

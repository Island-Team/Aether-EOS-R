/// eosd — thin CLI and signal entrypoint for the composed EOS runtime.

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string>

#include "src/eos/lib/log.h"
#include "src/eos/runtime/init/runtime.h"

namespace {
volatile std::sig_atomic_t g_stop = 0;
void HandleSignal(int) { g_stop = 1; }

std::uint16_t ParsePort(const std::string& text, const char* flag) {
  std::size_t consumed = 0;
  unsigned long value = 0;
  try {
    value = std::stoul(text, &consumed);
  } catch (const std::exception&) {
    throw std::runtime_error(std::string(flag) + " must be an integer in (0, 65535]");
  }
  if (consumed != text.size() || value == 0 || value > 65535) {
    throw std::runtime_error(std::string(flag) + " must be an integer in (0, 65535]");
  }
  return static_cast<std::uint16_t>(value);
}

eos::runtime::init::Options ParseArgs(int argc, char** argv) {
  eos::runtime::init::Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--config" && i + 1 < argc)
      options.config = argv[++i];
    else if (arg == "--etc" && i + 1 < argc)
      options.etc = argv[++i];
    else if (arg == "--contracts" && i + 1 < argc)
      options.contracts = argv[++i];
    else if (arg == "--skills-dir" && i + 1 < argc)
      options.skills_dir = argv[++i];
    else if (arg == "--host" && i + 1 < argc)
      options.host = argv[++i];
    else if (arg == "--grpc-port" && i + 1 < argc)
      options.grpc_port = ParsePort(argv[++i], "--grpc-port");
    else if (arg == "--log-level" && i + 1 < argc)
      options.log_level = argv[++i];
    else if (arg == "--no-prelaunch")
      options.no_prelaunch = true;
    else if (arg == "--help") {
      std::fprintf(stderr,
                   "usage: eosd [--config PATH] [--etc PATH] [--contracts PATH] "
                   "[--skills-dir PATH] [--host ADDR] [--grpc-port N] "
                   "[--log-level LEVEL] [--no-prelaunch]\n");
      std::exit(0);
    } else
      throw std::runtime_error("unknown argument: " + arg);
  }
  return options;
}
}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);
  try {
    return eos::runtime::init::Run(ParseArgs(argc, argv), [] { return g_stop != 0; });
  } catch (const std::exception& error) {
    try {
      eos::lib::ConfigureLogging(eos::lib::DefaultLogDirectory(), "eosd", eos::lib::LogConfig{});
      eos::lib::LogEvent(eos::lib::GetLogger("eosd"), spdlog::level::err, "daemon_start_failed",
                         {{"reason", error.what()}});
    } catch (...) {
      // Startup still fails cleanly when even the default file sink is unavailable.
    }
    return 1;
  }
}

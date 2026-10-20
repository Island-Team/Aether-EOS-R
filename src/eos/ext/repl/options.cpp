#include "options.h"

#include <charconv>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <toml++/toml.hpp>

#include "src/eos/lib/log.h"

namespace eos::ext::repl {
namespace {

const char* RequireValue(int argc, char** argv, int* index, const std::string& option) {
  if (*index + 1 >= argc || std::string(argv[*index + 1]).rfind("--", 0) == 0) {
    throw std::runtime_error(option + " requires a value");
  }
  return argv[++*index];
}

template <typename Integer>
Integer ParseBoundedPositiveInteger(const std::string& text, Integer maximum,
                                    const std::string& error) {
  Integer value = 0;
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto parsed = std::from_chars(begin, end, value);
  if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != end || value <= 0 ||
      value > maximum) {
    throw std::runtime_error(error);
  }
  return value;
}

std::uint16_t ParsePort(const std::string& text) {
  return ParseBoundedPositiveInteger<std::uint16_t>(text, std::numeric_limits<std::uint16_t>::max(),
                                                    "port must be an integer in (0, 65535]");
}

int ParseTimeout(const std::string& text, const std::string& option) {
  return ParseBoundedPositiveInteger<int>(text, std::numeric_limits<int>::max(),
                                          option + " must be a positive integer");
}

}  // namespace

const char* Usage() {
  return "usage: eos-repl [--host ADDR] [--port N] "
         "[--config PATH] [--contracts PATH] [--connect-timeout-ms N] "
         "[--timeout-ms N] [--eosd-pid PID] [--no-history]\n";
}

void LoadNorthHttpDefaults(const std::string& path, Args* args) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return;
  try {
    const auto table = toml::parse(in, path);
    if (const auto north_http = table["north_http"].as_table()) {
      if (const auto host = (*north_http)["host"].value<std::string>()) args->host = *host;
      if (const auto port = (*north_http)["port"].value<std::int64_t>();
          port && *port > 0 && *port <= 65535) {
        args->port = static_cast<std::uint16_t>(*port);
      }
    }
  } catch (const std::exception& error) {
    eos::lib::LogEvent(eos::lib::GetLogger("eos-repl"), spdlog::level::debug,
                       "repl_config_defaults_failed", {{"path", path}, {"reason", error.what()}});
  }
}

Args ParseArgs(int argc, char** argv, bool* help_requested) {
  Args args;
  bool have_host = false;
  bool have_port = false;
  bool help = false;
  for (int i = 1; i < argc; ++i) {
    const std::string option = argv[i];
    if (option == "--host") {
      args.host = RequireValue(argc, argv, &i, option);
      have_host = true;
    } else if (option == "--port") {
      args.port = ParsePort(RequireValue(argc, argv, &i, option));
      have_port = true;
    } else if (option == "--config") {
      args.config = RequireValue(argc, argv, &i, option);
    } else if (option == "--timeout-ms") {
      args.timeout_ms = ParseTimeout(RequireValue(argc, argv, &i, option), option);
    } else if (option == "--connect-timeout-ms") {
      args.connect_timeout_ms = ParseTimeout(RequireValue(argc, argv, &i, option), option);
    } else if (option == "--contracts") {
      args.contracts = RequireValue(argc, argv, &i, option);
    } else if (option == "--eosd-pid") {
      args.eosd_pid = ParseBoundedPositiveInteger<int>(
          RequireValue(argc, argv, &i, option), std::numeric_limits<int>::max(),
          option + " must be a positive integer");
    } else if (option == "--no-history") {
      args.history = false;
    } else if (option == "--help") {
      help = true;
    } else {
      throw std::runtime_error("unknown option '" + option + "'");
    }
  }

  const std::string explicit_host = args.host;
  const std::uint16_t explicit_port = args.port;
  LoadNorthHttpDefaults(args.config, &args);
  if (have_host) args.host = explicit_host;
  if (have_port) args.port = explicit_port;
  if (help_requested != nullptr) *help_requested = help;
  return args;
}

}  // namespace eos::ext::repl

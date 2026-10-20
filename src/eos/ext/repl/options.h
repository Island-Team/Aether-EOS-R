/// Command-line and north HTTP defaults for eos-repl.
#pragma once

#include <cstdint>
#include <string>

namespace eos::ext::repl {

struct Args {
  std::string host = "127.0.0.1";
  std::uint16_t port = 16005;
  std::string config = "etc/eos/north-http.toml";
  std::string contracts;
  int connect_timeout_ms = 2000;
  int timeout_ms = 630000;
  int eosd_pid = 0;
  bool history = true;
};

/// Applies optional `[north_http]` host/port defaults. Missing or malformed files
/// leave `args` unchanged because eos-repl must remain usable without config.
void LoadAdapterDefaults(const std::string& path, Args* args);

/// Parses the complete command line, then applies config defaults beneath any
/// explicit host/port overrides. Throws std::runtime_error on invalid input.
Args ParseArgs(int argc, char** argv, bool* help_requested = nullptr);

const char* Usage();

}  // namespace eos::ext::repl

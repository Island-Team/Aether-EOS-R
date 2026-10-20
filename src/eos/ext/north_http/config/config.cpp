/// Standalone north_http configuration parsing.
#include "src/eos/ext/north_http/config/config.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <toml++/toml.hpp>

namespace eos::ext::north_http {
namespace {

[[noreturn]] void Fail(const std::string& path, const std::string& message) {
  throw std::runtime_error("North HTTP config '" + path + "': " + message);
}

template <typename T>
T Positive(const toml::table& table, const char* key, T fallback, const std::string& path) {
  const auto value = table[key].value<std::int64_t>();
  if (!value) {
    if (table.contains(key)) Fail(path, std::string("[north_http] ") + key + " must be an integer");
    return fallback;
  }
  if (*value <= 0 || static_cast<std::uint64_t>(*value) >
                         static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
    Fail(path, std::string("[north_http] ") + key + " must be a positive integer");
  }
  return static_cast<T>(*value);
}

template <typename T>
std::optional<T> Typed(const toml::table& table, const char* key, const std::string& path,
                       const std::string& section, const char* type) {
  if (!table.contains(key)) return std::nullopt;
  const auto value = table[key].value<T>();
  if (!value) Fail(path, section + " " + key + " must be " + type);
  return value;
}

void AdmitKeys(const toml::table& table, const std::vector<std::string>& keys,
               const std::string& path, const std::string& section) {
  for (const auto& [key, value] : table) {
    (void)value;
    const std::string text(key.str());
    if (std::find(keys.begin(), keys.end(), text) == keys.end()) {
      Fail(path, section + " has unknown key '" + text + "'");
    }
  }
}

}  // namespace

bool IsLoopbackHost(const std::string& host) {
  return host == "127.0.0.1" || host == "::1" || host == "localhost";
}

void ValidateUpstream(const std::string& upstream) {
  if (upstream.empty()) throw std::runtime_error("upstream must be non-empty");
  if (upstream.rfind("unix:", 0) == 0 &&
      !std::filesystem::path(upstream.substr(5)).is_absolute()) {
    throw std::runtime_error("unix upstream must contain an absolute socket path");
  }
}

Config LoadConfig(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) Fail(path, "cannot open file for reading");
  std::ostringstream contents;
  contents << input.rdbuf();
  toml::table root;
  try {
    root = toml::parse(contents.str(), path);
  } catch (const toml::parse_error& error) {
    Fail(path, std::string(error.description()));
  }
  AdmitKeys(root, {"north_http", "action_list_audit", "log"}, path, "root");
  const auto* north_http = root["north_http"].as_table();
  if (!north_http) Fail(path, "[north_http] must be a table");
  AdmitKeys(
      *north_http,
      {"host", "port", "upstream", "allow_remote", "allowed_origins", "max_request_bytes",
       "max_connections", "max_inflight_requests", "max_sse_streams", "max_monitor_streams",
       "connect_timeout_ms", "request_timeout_ms", "sse_heartbeat_ms"},
      path, "[north_http]");

  Config config;
  if (const auto value = Typed<std::string>(*north_http, "host", path, "[north_http]", "a string"))
    config.host = *value;
  if (config.host.empty()) Fail(path, "[north_http] host must be non-empty");
  config.port = Positive<std::uint16_t>(*north_http, "port", config.port, path);
  if (const auto value =
          Typed<std::string>(*north_http, "upstream", path, "[north_http]", "a string"))
    config.upstream = *value;
  try {
    ValidateUpstream(config.upstream);
  } catch (const std::runtime_error& error) {
    Fail(path, "[north_http] " + std::string(error.what()));
  }
  if (const auto value =
          Typed<bool>(*north_http, "allow_remote", path, "[north_http]", "a boolean"))
    config.allow_remote = *value;
  if (!config.allow_remote && !IsLoopbackHost(config.host)) {
    Fail(path, "[north_http] non-loopback host requires allow_remote = true");
  }
  if (const auto* origins = (*north_http)["allowed_origins"].as_array()) {
    config.allowed_origins.clear();
    for (const auto& origin : *origins) {
      const auto value = origin.value<std::string>();
      if (!value || value->empty()) {
        Fail(path, "[north_http] allowed_origins entries must be non-empty strings");
      }
      config.allowed_origins.push_back(*value);
    }
  } else if ((*north_http).contains("allowed_origins")) {
    Fail(path, "[north_http] allowed_origins must be an array");
  }
  config.max_request_bytes =
      Positive<std::size_t>(*north_http, "max_request_bytes", config.max_request_bytes, path);
  config.max_connections =
      Positive<std::size_t>(*north_http, "max_connections", config.max_connections, path);
  config.max_inflight_requests = Positive<std::size_t>(*north_http, "max_inflight_requests",
                                                        config.max_inflight_requests, path);
  config.max_sse_streams =
      Positive<std::size_t>(*north_http, "max_sse_streams", config.max_sse_streams, path);
  config.max_monitor_streams = Positive<std::size_t>(
      *north_http, "max_monitor_streams", config.max_monitor_streams, path);
  if (config.max_connections > kMaximumConnectionCapacity)
    Fail(path, "[north_http] max_connections exceeds operational maximum");
  if (config.max_inflight_requests > kMaximumConnectionCapacity)
    Fail(path, "[north_http] max_inflight_requests exceeds operational maximum");
  if (config.max_sse_streams > kMaximumConnectionCapacity)
    Fail(path, "[north_http] max_sse_streams exceeds operational maximum");
  if (config.max_monitor_streams > kMaximumConnectionCapacity)
    Fail(path, "[north_http] max_monitor_streams exceeds operational maximum");
  if (config.max_inflight_requests > config.max_connections ||
      config.max_sse_streams > config.max_connections - config.max_inflight_requests ||
      config.max_monitor_streams >
          config.max_connections - config.max_inflight_requests - config.max_sse_streams) {
    Fail(path, "[north_http] request-class capacities exceed max_connections");
  }
  config.connect_timeout_ms =
      Positive<int>(*north_http, "connect_timeout_ms", config.connect_timeout_ms, path);
  config.request_timeout_ms =
      Positive<int>(*north_http, "request_timeout_ms", config.request_timeout_ms, path);
  if (config.request_timeout_ms > kMaximumRequestTimeoutMs) {
    Fail(path, "[north_http] request_timeout_ms must not exceed " +
                   std::to_string(kMaximumRequestTimeoutMs));
  }
  config.sse_heartbeat_ms =
      Positive<int>(*north_http, "sse_heartbeat_ms", config.sse_heartbeat_ms, path);

  if (const auto* audit = root["action_list_audit"].as_table()) {
    AdmitKeys(*audit, {"enabled", "rotation_size_bytes", "rotation_files", "reconnect_delay_ms"},
              path, "[action_list_audit]");
    if (const auto value = Typed<bool>(*audit, "enabled", path, "[action_list_audit]", "a boolean"))
      config.action_list_audit.enabled = *value;
    config.action_list_audit.rotation_size_bytes = Positive<std::uint64_t>(
        *audit, "rotation_size_bytes", config.action_list_audit.rotation_size_bytes, path);
    config.action_list_audit.rotation_files = Positive<std::uint32_t>(
        *audit, "rotation_files", config.action_list_audit.rotation_files, path);
    config.action_list_audit.reconnect_delay_ms = Positive<int>(
        *audit, "reconnect_delay_ms", config.action_list_audit.reconnect_delay_ms, path);
    if (config.action_list_audit.rotation_size_bytes > kMaximumAuditRotationSizeBytes)
      Fail(path, "[action_list_audit] rotation_size_bytes exceeds operational maximum");
    if (config.action_list_audit.rotation_files > kMaximumAuditRotationFiles)
      Fail(path, "[action_list_audit] rotation_files exceeds operational maximum");
    if (config.action_list_audit.rotation_files >
        kMaximumAuditRetainedBytes / config.action_list_audit.rotation_size_bytes)
      Fail(path, "[action_list_audit] retained encoded bytes exceed operational maximum");
    if (config.action_list_audit.reconnect_delay_ms > kMaximumAuditReconnectDelayMs)
      Fail(path, "[action_list_audit] reconnect_delay_ms exceeds operational maximum");
  } else if (root.contains("action_list_audit")) {
    Fail(path, "[action_list_audit] must be a table");
  }

  if (const auto* log = root["log"].as_table()) {
    AdmitKeys(*log,
              {"level", "file_level", "jsonl_level", "overwrite", "dedup", "dedup_window_s",
               "save_attachments", "rotation_size_bytes", "rotation_files"},
              path, "[log]");
    if (const auto value = Typed<std::string>(*log, "level", path, "[log]", "a string"))
      config.log.console_level = *value;
    if (const auto value = Typed<std::string>(*log, "file_level", path, "[log]", "a string"))
      config.log.file_level = *value;
    if (const auto value = Typed<std::string>(*log, "jsonl_level", path, "[log]", "a string"))
      config.log.jsonl_level = *value;
    if (const auto value = Typed<bool>(*log, "overwrite", path, "[log]", "a boolean"))
      config.log.overwrite = *value;
    if (const auto value = Typed<bool>(*log, "dedup", path, "[log]", "a boolean"))
      config.log.dedup = *value;
    if (const auto value = Typed<double>(*log, "dedup_window_s", path, "[log]", "a number"))
      config.log.dedup_window_s = *value;
    if (const auto value = Typed<bool>(*log, "save_attachments", path, "[log]", "a boolean"))
      config.log.save_attachments = *value;
    config.log.rotation_size_bytes =
        Positive<std::uint64_t>(*log, "rotation_size_bytes", config.log.rotation_size_bytes, path);
    config.log.rotation_files =
        Positive<std::uint32_t>(*log, "rotation_files", config.log.rotation_files, path);
  }
  return config;
}

}  // namespace eos::ext::north_http

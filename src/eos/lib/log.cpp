#include "src/eos/lib/log.h"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "spdlog/pattern_formatter.h"
#include "spdlog/sinks/ansicolor_sink.h"
#include "spdlog/sinks/base_sink.h"
#include "src/eos/lib/uuid.h"

namespace eos::lib {
namespace {

constexpr char kJsonlFileName[] = "events.jsonl";
constexpr char kLaunchIdEnv[] = "EOS_LAUNCH_ID";
constexpr std::size_t kMaxStringBytes = 1024;
constexpr std::size_t kMaxEventBytes = 128;
// Correlated lifecycle events normally have unique command/execution IDs, so
// deduplication state must stay bounded for long-running robot processes.
constexpr std::size_t kMaxDedupRecords = 4096;

thread_local std::string g_correlation_id;
LogProcessContext g_process_context;

struct StructuredContext {
  std::string event;
  nlohmann::json fields;
  std::string summary;
};
thread_local const StructuredContext* g_structured_context = nullptr;

void AppendJsonEscaped(std::string& out, const spdlog::string_view_t value) {
  for (const char c : std::string(value.data(), value.size())) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
          out += buf;
        } else {
          out += c;
        }
    }
  }
}

bool IsSensitiveKey(std::string key) {
  std::transform(key.begin(), key.end(), key.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  static constexpr const char* kSensitive[] = {"password", "passwd",        "secret",
                                               "token",    "authorization", "credential",
                                               "api_key",  "apikey",        "cookie"};
  for (const char* word : kSensitive) {
    if (key.find(word) != std::string::npos) return true;
  }
  return false;
}

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWith(const std::string& value, const std::string& suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string NormalizeKey(std::string key) {
  std::string normalized;
  normalized.reserve(key.size());
  bool separator = false;
  unsigned char previous = 0;
  for (const unsigned char c : key) {
    if (std::isalnum(c)) {
      if (std::isupper(c) && !normalized.empty() && !separator &&
          (std::islower(previous) || std::isdigit(previous))) {
        normalized.push_back('_');
      }
      normalized.push_back(static_cast<char>(std::tolower(c)));
      separator = false;
    } else if (!normalized.empty() && !separator) {
      normalized.push_back('_');
      separator = true;
    }
    previous = c;
  }
  if (!normalized.empty() && normalized.back() == '_') normalized.pop_back();
  return normalized;
}

bool HasKeySegment(const std::string& key, std::string_view segment) {
  std::size_t begin = 0;
  while (begin <= key.size()) {
    const std::size_t end = key.find('_', begin);
    const std::size_t size = end == std::string::npos ? key.size() - begin : end - begin;
    if (key.compare(begin, size, segment) == 0) return true;
    if (end == std::string::npos) return false;
    begin = end + 1;
  }
  return false;
}

bool IsSafeContentMetadata(const std::string& key, const nlohmann::json& value) {
  if (value.is_boolean() &&
      (key == "speech" || key == "speech_active" || key == "speech_detected" ||
       key == "speech_started" || key == "speech_ended"))
    return true;
  if (value.is_number() && (key == "audio_events" || key == "audio_event_count")) return true;
  static constexpr std::string_view kNumericSuffixes[] = {
      "bytes",  "byte_count", "size",        "size_bytes",     "count",      "width",
      "height", "channels",   "sample_rate", "sample_rate_hz", "duration_ms"};
  if (value.is_number()) {
    for (const std::string_view suffix : kNumericSuffixes) {
      if (key == suffix || EndsWith(key, "_" + std::string(suffix))) return true;
    }
  }
  static constexpr std::string_view kDescriptorSuffixes[] = {
      "id", "type", "media_type", "format", "encoding", "status", "version", "device"};
  if (value.is_string()) {
    for (const std::string_view suffix : kDescriptorSuffixes) {
      if (key == suffix || EndsWith(key, "_" + std::string(suffix))) return true;
    }
  }
  return false;
}

bool IsContentBearingKey(const std::string& key, const nlohmann::json& value) {
  const std::string normalized = NormalizeKey(key);
  if (normalized.empty() || IsSafeContentMetadata(normalized, value)) return false;
  static constexpr std::string_view kContentSegments[] = {
      "payload", "body",    "argument", "arguments",  "args",        "result", "results", "schema",
      "schemas", "content", "contents", "transcript", "transcripts", "prompt", "prompts", "image",
      "images",  "audio",   "speech",   "media",      "data",        "blob",   "base64",  "b64"};
  return std::any_of(
      std::begin(kContentSegments), std::end(kContentSegments),
      [&](const std::string_view segment) { return HasKeySegment(normalized, segment); });
}

bool IsEndpointKey(std::string key) {
  std::transform(key.begin(), key.end(), key.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return key == "endpoint" || key == "url" || key == "uri" || EndsWith(key, "_endpoint") ||
         EndsWith(key, "_url") || EndsWith(key, "_uri");
}

std::string SanitizeString(std::string value) {
  for (char& c : value) {
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = ' ';
  }
  if (value.size() > kMaxStringBytes) {
    value.resize(kMaxStringBytes);
    value += "...";
  }
  return value;
}

std::string SanitizeEndpoint(std::string value) {
  value = SanitizeString(std::move(value));
  const std::size_t scheme = value.find("://");
  if (scheme != std::string::npos) {
    const std::size_t authority = scheme + 3;
    const std::size_t path = value.find('/', authority);
    const std::size_t end_authority = path == std::string::npos ? value.size() : path;
    const std::size_t at = value.rfind('@', end_authority);
    if (at != std::string::npos && at >= authority) value.erase(authority, at - authority + 1);
  }
  const std::size_t suffix = value.find_first_of("?#");
  if (suffix != std::string::npos) value.erase(suffix);
  return value;
}

nlohmann::json SanitizeJsonImpl(const nlohmann::json& value, const std::string& key = "") {
  if (IsSensitiveKey(key) || IsContentBearingKey(key, value)) return "[REDACTED]";
  if (value.is_object()) {
    nlohmann::json result = nlohmann::json::object();
    for (auto it = value.begin(); it != value.end(); ++it)
      result[it.key()] = SanitizeJsonImpl(it.value(), it.key());
    return result;
  }
  if (value.is_array()) {
    nlohmann::json result = nlohmann::json::array();
    for (const auto& item : value) result.push_back(SanitizeJsonImpl(item));
    return result;
  }
  if (value.is_string()) {
    std::string text = value.get<std::string>();
    return IsEndpointKey(key) ? SanitizeEndpoint(std::move(text)) : SanitizeString(std::move(text));
  }
  return value;
}

std::string SanitizeEventImpl(std::string event) {
  event = SanitizeString(std::move(event));
  if (event.size() > kMaxEventBytes) event.resize(kMaxEventBytes);
  return event;
}

std::string HumanMessage(const StructuredContext& context) {
  std::string result = context.summary;
  if (result.empty()) {
    result = context.event;
    std::replace(result.begin(), result.end(), '_', ' ');
    if (!result.empty()) {
      result.front() =
          static_cast<char>(std::toupper(static_cast<unsigned char>(result.front())));
    }
  }
  // A reason is useful before IDs, codes, and other diagnostic metadata.
  std::string reason_key;
  if (context.fields.is_object()) {
    for (const char* key : {"reason", "error", "failure_reason"}) {
      if (context.fields.contains(key) && context.fields.at(key).is_string()) {
        reason_key = key;
        result += ": " + context.fields.at(key).get<std::string>();
        break;
      }
    }
  }
  result += " [" + context.event + "]";
  if (context.fields.is_object()) {
    for (auto it = context.fields.begin(); it != context.fields.end(); ++it) {
      if (it.key() == reason_key) continue;
      result += " " + it.key() + "=";
      result += it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
    }
  } else if (!context.fields.empty()) {
    result += " fields=" + context.fields.dump();
  }
  return result;
}

class DedupTracker {
 public:
  DedupTracker(bool enabled, double window_s)
      : enabled_(enabled), window_(std::chrono::duration<double>(window_s)) {}

  bool Admit(const spdlog::details::log_msg& msg, std::size_t* repeats) {
    *repeats = 0;
    if (!enabled_) return true;
    std::string key(msg.logger_name.data(), msg.logger_name.size());
    key += '\0';
    key += std::to_string(static_cast<int>(msg.level));
    key += '\0';
    key.append(msg.payload.data(), msg.payload.size());
    const auto now = std::chrono::steady_clock::now();
    auto it = records_.find(key);
    if (it == records_.end()) {
      if (records_.size() >= kMaxDedupRecords) records_.erase(records_.begin());
      records_.emplace(std::move(key), Record{now, now, 0});
      return true;
    }
    const bool repeated = now - it->second.last_seen < window_;
    it->second.last_seen = now;
    // An ongoing fault must stay visible even if each occurrence refreshes
    // the quiet period. Less severe messages retain quiet-period semantics.
    const bool periodic_fault = msg.level >= spdlog::level::warn &&
                                now - it->second.last_emitted >= window_;
    if (repeated && !periodic_fault) {
      ++it->second.suppressed;
      return false;
    }
    *repeats = it->second.suppressed;
    it->second = Record{now, now, 0};
    return true;
  }

 private:
  struct Record {
    std::chrono::steady_clock::time_point last_seen;
    std::chrono::steady_clock::time_point last_emitted;
    std::size_t suppressed;
  };
  bool enabled_;
  std::chrono::duration<double> window_;
  std::unordered_map<std::string, Record> records_;
};

class RotatingFile {
 public:
  RotatingFile(std::filesystem::path path, bool truncate, std::uint64_t maximum_bytes,
               std::uint32_t retained_files)
      : path_(std::move(path)), maximum_bytes_(maximum_bytes), retained_files_(retained_files) {
    std::error_code error;
    std::filesystem::create_directories(path_.parent_path(), error);
    if (error) {
      throw spdlog::spdlog_ex("cannot create log directory " + path_.parent_path().string() + ": " +
                              error.message());
    }
    if (truncate) {
      std::filesystem::remove(path_, error);
      if (error) throw spdlog::spdlog_ex("cannot clear log file: " + path_.string());
      for (std::uint32_t index = 1; index <= retained_files_; ++index) {
        std::filesystem::remove(RotatedPath(index), error);
        if (error) throw spdlog::spdlog_ex("cannot clear rotated log: " + path_.string());
      }
    }
    Open();
  }

  void Write(std::string_view data) {
    if (size_ != 0 && size_ + data.size() > maximum_bytes_) Rotate();
    file_.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!file_) throw spdlog::spdlog_ex("failed to write log file: " + path_.string());
    size_ += data.size();
  }

  void Flush() { file_.flush(); }

 private:
  std::filesystem::path RotatedPath(std::uint32_t index) const {
    return path_.string() + "." + std::to_string(index);
  }

  void Open() {
    std::error_code error;
    size_ = std::filesystem::exists(path_, error) ? std::filesystem::file_size(path_, error) : 0;
    if (error) throw spdlog::spdlog_ex("cannot inspect log file: " + path_.string());
    file_.open(path_, std::ios::binary | std::ios::out | std::ios::app);
    if (!file_) throw spdlog::spdlog_ex("failed to open log file: " + path_.string());
    if (size_ >= maximum_bytes_) Rotate();
  }

  void Rotate() {
    file_.close();
    std::error_code error;
    std::filesystem::remove(RotatedPath(retained_files_), error);
    if (error) throw spdlog::spdlog_ex("cannot remove rotated log: " + path_.string());
    for (std::uint32_t index = retained_files_; index > 1; --index) {
      const std::filesystem::path source = RotatedPath(index - 1);
      if (!std::filesystem::exists(source, error)) {
        if (error) throw spdlog::spdlog_ex("cannot inspect rotated log: " + source.string());
        continue;
      }
      std::filesystem::rename(source, RotatedPath(index), error);
      if (error) throw spdlog::spdlog_ex("cannot rotate log: " + source.string());
    }
    if (std::filesystem::exists(path_, error)) {
      if (error) throw spdlog::spdlog_ex("cannot inspect log file: " + path_.string());
      std::filesystem::rename(path_, RotatedPath(1), error);
      if (error) throw spdlog::spdlog_ex("cannot rotate log file: " + path_.string());
    }
    file_.clear();
    file_.open(path_, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!file_) throw spdlog::spdlog_ex("failed to open log file: " + path_.string());
    size_ = 0;
  }

  std::filesystem::path path_;
  std::uint64_t maximum_bytes_;
  std::uint32_t retained_files_;
  std::uint64_t size_{0};
  std::ofstream file_;
};

std::string RouteForLogger(std::string name) {
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (name == "eosd" || name == "main" || name == "eos-north-http" || name == "north-http" ||
      StartsWith(name, "eos.main"))
    return "main.log";
  if (name == "lib" || StartsWith(name, "lib.") || StartsWith(name, "eos.lib")) return "lib.log";
  const std::pair<const char*, const char*> runtime[] = {{"hardware", "hardware"},
                                                         {"dispatch", "dispatch"},
                                                         {"northbound", "northbound"},
                                                         {"body", "body"},
                                                         {"models", "models"},
                                                         {"skills", "skills"},
                                                         {"actionlist", "actionlist"},
                                                         {"contracts", "contracts"},
                                                         {"init", "init"}};
  for (const auto& [prefix, file] : runtime) {
    if (name == prefix || StartsWith(name, std::string(prefix) + ".") ||
        StartsWith(name, "eos." + std::string(prefix)))
      return "runtime-" + std::string(file) + ".log";
  }
  const std::pair<const char*, const char*> extensions[] = {
      {"north-http.audit", "north-http-audit"},
      {"north-http.client", "north-http-client"},
      {"eos-repl", "repl"},
      {"repl", "repl"},
      {"ros2", "ros2"},
      {"speaker", "speaker"},
      {"microphone", "alsa"},
      {"camera", "camera"},
      {"vendor", "vendor"},
      {"simulator", "simulator"},
      {"demo", "demo"}};
  for (const auto& [prefix, file] : extensions) {
    if (name == prefix || StartsWith(name, std::string(prefix) + ".") ||
        StartsWith(name, "eos." + std::string(prefix)))
      return "ext-" + std::string(file) + ".log";
  }
  return "thirdparty.log";
}

class RoutedTextSink final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  RoutedTextSink(std::filesystem::path root, bool truncate, bool dedup, double window_s,
                 std::uint64_t maximum_bytes, std::uint32_t retained_files)
      : root_(std::move(root)),
        truncate_(truncate),
        maximum_bytes_(maximum_bytes),
        retained_files_(retained_files),
        dedup_(dedup, window_s) {}

 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    std::size_t repeats = 0;
    if (!dedup_.Admit(msg, &repeats)) return;
    const std::string route =
        RouteForLogger(std::string(msg.logger_name.data(), msg.logger_name.size()));
    auto [it, inserted] = files_.try_emplace(route);
    if (inserted) {
      const std::filesystem::path path = root_ / route;
      it->second = std::make_unique<RotatingFile>(path, truncate_, maximum_bytes_, retained_files_);
    }
    spdlog::memory_buf_t formatted;
    if (repeats > 0) {
      std::string payload(msg.payload.data(), msg.payload.size());
      payload += " repeats=" + std::to_string(repeats);
      spdlog::details::log_msg repeated = msg;
      repeated.payload = spdlog::string_view_t(payload);
      formatter_->format(repeated, formatted);
    } else {
      formatter_->format(msg, formatted);
    }
    it->second->Write(std::string_view(formatted.data(), formatted.size()));
    it->second->Flush();
  }
  void flush_() override {
    for (auto& [name, file] : files_) file->Flush();
  }

 private:
  std::filesystem::path root_;
  bool truncate_;
  std::uint64_t maximum_bytes_;
  std::uint32_t retained_files_;
  DedupTracker dedup_;
  std::unordered_map<std::string, std::unique_ptr<RotatingFile>> files_;
};

class JsonlFileSink final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  JsonlFileSink(const std::filesystem::path& path, bool truncate, bool dedup, double window_s,
                std::uint64_t maximum_bytes, std::uint32_t retained_files)
      : file_(path, truncate, maximum_bytes, retained_files), dedup_(dedup, window_s) {}

 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    std::size_t repeats = 0;
    if (!dedup_.Admit(msg, &repeats)) return;
    const auto secs = std::chrono::time_point_cast<std::chrono::seconds>(msg.time);
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(msg.time - secs).count();
    const std::time_t t = std::chrono::system_clock::to_time_t(secs);
    std::tm tm;
    gmtime_r(&t, &tm);
    char ts[64];
    std::snprintf(ts, sizeof(ts), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<int>(millis));
    std::string line = "{\"ts\":\"";
    line += ts;
    line += "\",\"level\":\"";
    const auto level = spdlog::level::to_string_view(msg.level);
    line.append(level.data(), level.size());
    line += "\",\"logger\":\"";
    AppendJsonEscaped(line, msg.logger_name);
    line += "\",\"msg\":\"";
    AppendJsonEscaped(line, msg.payload);
    line += "\",\"service\":" + nlohmann::json(g_process_context.service).dump();
    line += ",\"pid\":" + std::to_string(g_process_context.pid);
    line += ",\"version\":" + nlohmann::json(g_process_context.version).dump();
    line += ",\"launch_id\":" + nlohmann::json(g_process_context.launch_id).dump();
    line += ",\"run_id\":" + nlohmann::json(g_process_context.run_id).dump();
    if (!g_correlation_id.empty()) {
      line += ",\"correlation_id\":\"";
      AppendJsonEscaped(line, g_correlation_id);
      line += "\"";
    }
    if (g_structured_context != nullptr) {
      line += ",\"event\":" + nlohmann::json(g_structured_context->event).dump();
      line += ",\"fields\":" + g_structured_context->fields.dump();
    }
    if (repeats > 0) line += ",\"repeats\":" + std::to_string(repeats);
    line += "}\n";
    file_.Write(line);
    file_.Flush();
  }
  void flush_() override { file_.Flush(); }

 private:
  RotatingFile file_;
  DedupTracker dedup_;
};

class UpperLevelFlag final : public spdlog::custom_flag_formatter {
 public:
  void format(const spdlog::details::log_msg& msg, const std::tm&,
              spdlog::memory_buf_t& dest) override {
    const auto level = spdlog::level::to_string_view(msg.level);
    for (const char c : std::string(level.data(), level.size()))
      dest.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  std::unique_ptr<custom_flag_formatter> clone() const override {
    return spdlog::details::make_unique<UpperLevelFlag>();
  }
};

spdlog::level::level_enum ParseLevel(std::string level) {
  for (char& c : level) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  const auto parsed = spdlog::level::from_str(level);
  if (parsed == spdlog::level::off && level != "off")
    throw spdlog::spdlog_ex("unknown log level: " + level);
  return parsed;
}

bool IsManagedLogFile(const std::filesystem::path& path) {
  const std::string name = path.filename().string();
  return name == kJsonlFileName || StartsWith(name, std::string(kJsonlFileName) + ".") ||
         EndsWith(name, ".log") || name.find(".log.") != std::string::npos;
}

void RemoveStaleLogs(const std::filesystem::path& log_dir) {
  std::error_code ec;
  std::vector<std::filesystem::path> stale;
  for (std::filesystem::recursive_directory_iterator it(log_dir, ec), end; it != end && !ec;
       it.increment(ec)) {
    if (it->is_regular_file(ec) && !ec && IsManagedLogFile(it->path())) {
      stale.push_back(it->path());
    }
  }
  if (ec) {
    throw spdlog::spdlog_ex("cannot inspect log dir " + log_dir.string() + ": " + ec.message());
  }
  for (const auto& path : stale) {
    std::filesystem::remove(path, ec);
    if (ec) {
      throw spdlog::spdlog_ex("cannot remove stale log " + path.string() + ": " + ec.message());
    }
  }
}

struct Sinks {
  std::shared_ptr<spdlog::sinks::ansicolor_stderr_sink_mt> console;
  std::shared_ptr<RoutedTextSink> text;
  std::shared_ptr<JsonlFileSink> jsonl;
};

std::unique_ptr<spdlog::pattern_formatter> MakeTextFormatter() {
  auto formatter = std::make_unique<spdlog::pattern_formatter>("", spdlog::pattern_time_type::utc);
  formatter->add_flag<UpperLevelFlag>('U').set_pattern(
      "[aether-eos]%U@%n[%Y-%m-%dT%H:%M:%S.%eZ]: %v");
  return formatter;
}

Sinks& GetSinks() {
  static Sinks sinks = [] {
    Sinks s;
    s.console = std::make_shared<spdlog::sinks::ansicolor_stderr_sink_mt>();
    auto formatter =
        std::make_unique<spdlog::pattern_formatter>("", spdlog::pattern_time_type::utc);
    formatter->add_flag<UpperLevelFlag>('U').set_pattern(
        "[aether-eos]%^%U%$@%n[%Y-%m-%dT%H:%M:%S.%eZ]: %v");
    s.console->set_formatter(std::move(formatter));
    s.console->set_level(spdlog::level::off);
    return s;
  }();
  return sinks;
}

std::mutex& LoggerMutex() {
  static std::mutex mutex;
  return mutex;
}
std::mutex& OnceMutex() {
  static std::mutex mutex;
  return mutex;
}

struct AttachmentConfig {
  std::mutex mutex;
  std::filesystem::path root;
  std::unordered_map<std::string, std::vector<std::filesystem::path>> files_by_content;
};

AttachmentConfig& GetAttachmentConfig() {
  static AttachmentConfig config;
  return config;
}

bool IsSafeAttachmentComponent(const std::string& value, bool allow_dot) {
  if (value.empty()) return false;
  return std::all_of(value.begin(), value.end(), [allow_dot](unsigned char c) {
    return std::isalnum(c) || c == '-' || c == '_' || (allow_dot && c == '.');
  });
}

std::string AttachmentContentKey(const std::string& category, const std::string& extension,
                                 const std::string& media_type,
                                 const std::vector<std::uint8_t>& data) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const std::uint8_t byte : data) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return category + '\x1f' + extension + '\x1f' + media_type + '\x1f' +
         std::to_string(data.size()) + '\x1f' + std::to_string(hash);
}

bool AttachmentEquals(const std::filesystem::path& path, const std::vector<std::uint8_t>& data) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error ||
      std::filesystem::file_size(path, error) != data.size() || error) {
    return false;
  }
  std::ifstream input(path, std::ios::binary);
  std::vector<std::uint8_t> buffer(64 * 1024);
  std::size_t offset = 0;
  while (input && offset < data.size()) {
    const std::size_t size = std::min(buffer.size(), data.size() - offset);
    input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(size));
    const std::size_t read = static_cast<std::size_t>(input.gcount());
    if (read != size || !std::equal(buffer.begin(), buffer.begin() + read, data.begin() + offset)) {
      return false;
    }
    offset += read;
  }
  return offset == data.size();
}

nlohmann::json WriteAttachment(const std::string& category, const std::string& extension,
                               const std::string& media_type,
                               const std::vector<std::uint8_t>& data) {
  if (!IsSafeAttachmentComponent(category, false) || !IsSafeAttachmentComponent(extension, true) ||
      extension.front() != '.') {
    throw std::invalid_argument("invalid attachment path component");
  }
  AttachmentConfig& config = GetAttachmentConfig();
  std::lock_guard<std::mutex> lock(config.mutex);
  if (config.root.empty()) return nullptr;
  const std::string content_key = AttachmentContentKey(category, extension, media_type, data);
  auto& matching_files = config.files_by_content[content_key];
  for (const std::filesystem::path& existing : matching_files) {
    if (AttachmentEquals(existing, data)) {
      return {{"path", existing.string()},
              {"relative_path", (config.root.filename() / existing.filename()).generic_string()},
              {"category", category},
              {"media_type", media_type},
              {"size_bytes", data.size()},
              {"duplicate", true}};
    }
  }
  std::error_code error;
  std::filesystem::create_directories(config.root, error);
  if (error) throw std::runtime_error("cannot create attachment directory: " + error.message());
  const std::filesystem::path path = config.root / (category + "-" + MintUuidV7() + extension);
  const std::filesystem::path temporary = path.string() + ".tmp";
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));
  output.close();
  if (!output) {
    std::filesystem::remove(temporary, error);
    throw std::runtime_error("cannot write attachment " + path.string());
  }
  std::filesystem::rename(temporary, path, error);
  if (error) {
    const std::string reason = error.message();
    std::error_code remove_error;
    std::filesystem::remove(temporary, remove_error);
    throw std::runtime_error("cannot publish attachment " + path.string() + ": " + reason);
  }
  matching_files.push_back(path);
  return {{"path", path.string()},
          {"relative_path", (config.root.filename() / path.filename()).generic_string()},
          {"category", category},
          {"media_type", media_type},
          {"size_bytes", data.size()}};
}
std::unordered_set<std::string>& OnceKeys() {
  static std::unordered_set<std::string> keys;
  return keys;
}

std::string MintCorrelationId() {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::uniform_int_distribution<std::uint64_t> dist;
  const std::uint64_t hi = dist(gen), lo = dist(gen);
  char buf[37];
  std::snprintf(buf, sizeof(buf), "%08llx-%04llx-%04llx-%04llx-%012llx",
                static_cast<unsigned long long>(hi >> 32),
                static_cast<unsigned long long>((hi >> 16) & 0xFFFF),
                static_cast<unsigned long long>((hi & 0x0FFF) | 0x4000),
                static_cast<unsigned long long>(((lo >> 48) & 0x3FFF) | 0x8000),
                static_cast<unsigned long long>(lo & 0xFFFFFFFFFFFF));
  return buf;
}

std::vector<spdlog::sink_ptr> CurrentSinks() {
  Sinks& sinks = GetSinks();
  std::vector<spdlog::sink_ptr> result{sinks.console};
  if (sinks.text) result.push_back(sinks.text);
  if (sinks.jsonl) result.push_back(sinks.jsonl);
  return result;
}

void UpdateLatestRunLink(const std::filesystem::path& root, const std::string& launch_id) {
  std::error_code error;
  std::filesystem::create_directories(root, error);
  if (error)
    throw spdlog::spdlog_ex("cannot create log root " + root.string() + ": " + error.message());
  const std::filesystem::path latest = root / "latest";
  const std::filesystem::path temporary =
      root / (".latest-" + std::to_string(static_cast<std::uint64_t>(::getpid())));
  std::filesystem::remove(temporary, error);
  error.clear();
  std::filesystem::create_directory_symlink(std::filesystem::path("runs") / launch_id, temporary,
                                            error);
  if (error) throw spdlog::spdlog_ex("cannot create latest log link: " + error.message());
  std::filesystem::rename(temporary, latest, error);
  if (error) {
    std::filesystem::remove(temporary);
    throw spdlog::spdlog_ex("cannot update latest log link: " + error.message());
  }
}

}  // namespace

std::filesystem::path DefaultLogDirectory() {
  const auto absolute = [](const char* name, const char* value) {
    const std::filesystem::path path(value);
    if (!path.is_absolute()) {
      throw std::invalid_argument(std::string(name) + " must be absolute: " + value);
    }
    return path.lexically_normal();
  };
  const auto environment = [](const char* name) -> const char* {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? value : nullptr;
  };

  if (const char* root = environment("ISLAND_LOG")) return absolute("ISLAND_LOG", root) / "eos";
  if (const char* root = environment("EOS_VAR")) return absolute("EOS_VAR", root) / "log/eos";
  if (const char* mode = environment("ISLAND_MODE");
      mode != nullptr && std::string(mode) == "system")
    return "/var/log/island/eos";
  if (const char* prefix = environment("ISLAND_PREFIX"))
    return absolute("ISLAND_PREFIX", prefix) / "var/log/eos";
  return "var/log/eos";
}

std::filesystem::path ProcessLogDirectory(const std::filesystem::path& root,
                                          std::string_view service, bool overwrite) {
  const auto is_ascii_token = [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '_';
  };
  if (service.empty() || !std::all_of(service.begin(), service.end(), is_ascii_token)) {
    throw std::invalid_argument("log service must be a non-empty ASCII path component");
  }
  std::string launch_id = overwrite ? "current" : "";
  if (!overwrite) {
    const char* configured = std::getenv(kLaunchIdEnv);
    static const std::string standalone_launch_id = MintUuidV7();
    launch_id = configured != nullptr && configured[0] != '\0' ? configured : standalone_launch_id;
  }
  if (launch_id.size() > 128 ||
      !std::all_of(launch_id.begin(), launch_id.end(), is_ascii_token)) {
    throw std::invalid_argument(
        "EOS_LAUNCH_ID must contain 1..128 ASCII letters, digits, '-', or '_'");
  }
  return root / "runs" / launch_id / service;
}

void ConfigureLogging() { ConfigureLogging(DefaultLogDirectory(), "eosd", LogConfig{}); }
void ConfigureLogging(const LogConfig& config) {
  ConfigureLogging(DefaultLogDirectory(), "eosd", config);
}
const std::string& CurrentCorrelationId() { return g_correlation_id; }

bool IsValidRequestId(std::string_view value) {
  return !value.empty() && value.size() <= 96 &&
         std::all_of(value.begin(), value.end(), [](unsigned char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == ':' || c == '-';
         });
}

std::string ResolveRequestId(std::string_view value) {
  if (value.empty()) return MintCorrelationId();
  if (!IsValidRequestId(value)) {
    throw std::invalid_argument(
        "request ID must contain 1..96 ASCII letters, digits, '.', '_', ':', or '-'");
  }
  return std::string(value);
}

const LogProcessContext& CurrentLogProcessContext() { return g_process_context; }

CorrelationScope::CorrelationScope(std::string id)
    : id_(id.empty() ? MintCorrelationId() : std::move(id)), previous_(g_correlation_id) {
  g_correlation_id = id_;
}
CorrelationScope::~CorrelationScope() { g_correlation_id = previous_; }

void ConfigureLogging(const std::filesystem::path& log_dir, const LogConfig& config) {
  static std::once_flag once;
  std::call_once(once, [&] {
    const auto console_level = ParseLevel(config.console_level);
    const auto file_level = ParseLevel(config.file_level);
    const auto jsonl_level = ParseLevel(config.jsonl_level);
    if (!(config.dedup_window_s > 0.0)) throw spdlog::spdlog_ex("dedup_window_s must be positive");
    if (config.rotation_size_bytes == 0)
      throw spdlog::spdlog_ex("rotation_size_bytes must be positive");
    if (config.rotation_files == 0) throw spdlog::spdlog_ex("rotation_files must be positive");
    std::error_code ec;
    std::filesystem::create_directories(log_dir, ec);
    if (ec)
      throw spdlog::spdlog_ex("cannot create log dir " + log_dir.string() + ": " + ec.message());
    if (config.overwrite) RemoveStaleLogs(log_dir);
    if (g_process_context.service.empty()) {
      const std::string run_id = MintUuidV7();
      g_process_context = {"standalone", EOS_PROJECT_VERSION, run_id, run_id,
                           static_cast<std::uint64_t>(::getpid())};
    }
    std::lock_guard<std::mutex> lock(LoggerMutex());
    Sinks& sinks = GetSinks();
    sinks.text = std::make_shared<RoutedTextSink>(log_dir, config.overwrite, config.dedup,
                                                  config.dedup_window_s, config.rotation_size_bytes,
                                                  config.rotation_files);
    sinks.text->set_formatter(MakeTextFormatter());
    sinks.text->set_level(file_level);
    sinks.jsonl = std::make_shared<JsonlFileSink>(log_dir / kJsonlFileName, config.overwrite,
                                                  config.dedup, config.dedup_window_s,
                                                  config.rotation_size_bytes, config.rotation_files);
    sinks.jsonl->set_level(jsonl_level);
    sinks.console->set_level(console_level);
    const auto configured = CurrentSinks();
    spdlog::apply_all([&configured](const std::shared_ptr<spdlog::logger>& logger) {
      logger->sinks() = configured;
      logger->set_level(spdlog::level::trace);
    });
    auto logger = spdlog::get("eos");
    if (!logger) {
      logger = std::make_shared<spdlog::logger>("eos", configured.begin(), configured.end());
      logger->set_level(spdlog::level::trace);
    }
    spdlog::set_default_logger(logger);
  });
}

void ConfigureLogging(const std::filesystem::path& log_root, const std::string& service,
                      const LogConfig& config) {
  const std::filesystem::path process_dir = ProcessLogDirectory(log_root, service, config.overwrite);
  if (!g_process_context.service.empty()) {
    ConfigureLogging(process_dir, config);
    return;
  }
  const std::string launch_id = process_dir.parent_path().filename().string();
  g_process_context = {service, EOS_PROJECT_VERSION, launch_id, MintUuidV7(),
                       static_cast<std::uint64_t>(::getpid())};
  if (::setenv("ISLAND_LOG_DIR", process_dir.c_str(), 1) != 0) {
    g_process_context = {};
    throw spdlog::spdlog_ex("cannot export process log directory " + process_dir.string() + ": " +
                            std::strerror(errno));
  }
  try {
    UpdateLatestRunLink(log_root, launch_id);
    ConfigureLogging(process_dir, config);
  } catch (...) {
    ::unsetenv("ISLAND_LOG_DIR");
    g_process_context = {};
    throw;
  }
}

void ConfigureLogging(const std::filesystem::path& log_dir, const std::string& console_level) {
  LogConfig config;
  config.console_level = console_level;
  ConfigureLogging(log_dir, config);
}

void ConfigureLogAttachments(const std::filesystem::path& root, bool overwrite) {
  AttachmentConfig& config = GetAttachmentConfig();
  std::lock_guard<std::mutex> lock(config.mutex);
  if (root.empty()) {
    config.root.clear();
    config.files_by_content.clear();
    return;
  }
  if (root.filename() != "attachments" || root == root.root_path()) {
    throw std::invalid_argument("attachment root must be a dedicated directory named 'attachments'");
  }
  std::error_code error;
  if (overwrite) {
    std::filesystem::remove_all(root, error);
    if (error) {
      throw spdlog::spdlog_ex("cannot clear attachment dir " + root.string() + ": " +
                              error.message());
    }
  }
  std::filesystem::create_directories(root, error);
  if (error) {
    throw spdlog::spdlog_ex("cannot create attachment dir " + root.string() + ": " +
                            error.message());
  }
  config.root = root;
  config.files_by_content.clear();
}

std::shared_ptr<spdlog::logger> GetLogger(const std::string& name) {
  std::lock_guard<std::mutex> lock(LoggerMutex());
  if (auto existing = spdlog::get(name)) return existing;
  const auto sinks = CurrentSinks();
  auto logger = std::make_shared<spdlog::logger>(name, sinks.begin(), sinks.end());
  logger->set_level(spdlog::level::trace);
  spdlog::register_logger(logger);
  return logger;
}

nlohmann::json SanitizeLogFields(const nlohmann::json& fields) {
  return SanitizeJsonImpl(fields);
}

std::string SanitizeLogEvent(std::string event) {
  return SanitizeEventImpl(std::move(event));
}

void LogEvent(const std::shared_ptr<spdlog::logger>& logger, spdlog::level::level_enum level,
              const std::string& event, const nlohmann::json& fields,
              std::string_view summary) {
  if (!logger || !logger->should_log(level)) return;
  StructuredContext context{SanitizeLogEvent(event), SanitizeLogFields(fields),
                            SanitizeString(std::string(summary))};
  const StructuredContext* previous = g_structured_context;
  g_structured_context = &context;
  try {
    logger->log(level, HumanMessage(context));
  } catch (...) {
    g_structured_context = previous;
    throw;
  }
  g_structured_context = previous;
}

void LogEventWithAttachment(const std::shared_ptr<spdlog::logger>& logger,
                            spdlog::level::level_enum level, const std::string& event,
                            nlohmann::json fields, const std::string& category,
                            const std::string& extension, const std::string& media_type,
                            const std::vector<std::uint8_t>& data) {
  if (!logger || !logger->should_log(level)) return;
  try {
    nlohmann::json attachment = WriteAttachment(category, extension, media_type, data);
    if (!attachment.is_null()) fields["attachment"] = std::move(attachment);
  } catch (const std::exception& error) {
    fields["attachment_error"] = error.what();
  }
  LogEvent(logger, level, event, fields);
}

void LogOnce(const std::shared_ptr<spdlog::logger>& logger, spdlog::level::level_enum level,
             const std::string& key, const std::string& event, const nlohmann::json& fields) {
  if (!logger) return;
  const std::string identity = logger->name() + '\x1f' + key;
  {
    std::lock_guard<std::mutex> lock(OnceMutex());
    if (!OnceKeys().insert(identity).second) return;
  }
  LogEvent(logger, level, event, fields);
}

void SetStderrLevel(const std::string& level) { GetSinks().console->set_level(ParseLevel(level)); }

}  // namespace eos::lib

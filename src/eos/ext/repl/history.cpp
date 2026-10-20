#include "history.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>

namespace eos::ext::repl {
namespace {

bool AbsoluteEnv(const char* name, std::filesystem::path* value) {
  const char* raw = std::getenv(name);
  if (raw == nullptr || *raw == '\0') return false;
  const std::filesystem::path path(raw);
  if (!path.is_absolute()) return false;
  *value = path;
  return true;
}

}  // namespace

std::string ResolveHistoryPath() {
  std::filesystem::path root;
  if (AbsoluteEnv("XDG_STATE_HOME", &root)) return (root / "island/eos-repl/history").string();
  if (AbsoluteEnv("HOME", &root)) return (root / ".local/state/island/eos-repl/history").string();
  return {};
}

bool PrepareHistoryPath(const std::string& path, std::string* warning) {
  if (path.empty()) {
    if (warning) *warning = "HOME and XDG_STATE_HOME do not provide an absolute history location";
    return false;
  }
  std::error_code ec;
  const auto parent = std::filesystem::path(path).parent_path();
  std::filesystem::create_directories(parent, ec);
  const auto island = parent.parent_path();
  if (ec || (!island.empty() && ::chmod(island.c_str(), S_IRWXU) != 0) ||
      ::chmod(parent.c_str(), S_IRWXU) != 0) {
    if (warning) *warning = "cannot create private history directory: " + parent.string();
    return false;
  }
  const int file = ::open(path.c_str(), O_CREAT | O_APPEND | O_CLOEXEC, S_IRUSR | S_IWUSR);
  if (file < 0) {
    if (warning) *warning = "cannot create private history file: " + path;
    return false;
  }
  ::close(file);
  if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
    if (warning) *warning = "cannot secure history file: " + path;
    return false;
  }
  return true;
}

bool SecureHistoryFile(const std::string& path, std::string* warning) {
  if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) == 0) return true;
  if (warning) *warning = "cannot secure history file: " + path;
  return false;
}

}  // namespace eos::ext::repl

#include "terminal.h"

#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <poll.h>
#include <termios.h>

#include "history.h"
#include "src/eos/lib/log.h"

namespace eos::ext::repl {
namespace {

class ScopedRawMode final {
 public:
  ScopedRawMode() {
    if (::tcgetattr(STDIN_FILENO, &saved_) != 0) return;
    termios raw = saved_;
    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | ISIG));
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    active_ = ::tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
  }
  ~ScopedRawMode() {
    if (active_) ::tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
  }
  bool active() const { return active_; }

 private:
  termios saved_{};
  bool active_ = false;
};

}  // namespace

Terminal::Terminal(const CompletionEngine* completion, bool colors, bool history_enabled)
    : interactive_(::isatty(STDIN_FILENO) != 0 && ::isatty(STDOUT_FILENO) != 0),
      colors_(colors),
      history_enabled_(interactive_ && history_enabled) {
  if (!interactive_) return;
  editor_.set_max_history_size(1000);
  editor_.set_unique_history(true);
  editor_.set_completion_count_cutoff(80);
  editor_.set_max_hint_rows(1);
  editor_.set_hint_delay(250);
  editor_.set_complete_on_empty(true);
  editor_.set_beep_on_ambiguous_completion(false);
  editor_.set_indent_multiline(true);
  editor_.set_no_color(!colors_);
  editor_.enable_bracketed_paste();
  editor_.install_window_change_handler();
  editor_.set_completion_callback([completion](const std::string& input, int& context_length) {
    replxx::Replxx::completions_t result;
    for (const auto& value : completion->Complete(input, &context_length))
      result.emplace_back(value);
    return result;
  });
  editor_.set_hint_callback(
      [completion](const std::string& input, int& context_length, replxx::Replxx::Color& color) {
        replxx::Replxx::hints_t result;
        const std::string hint = completion->Hint(input, &context_length);
        if (!hint.empty()) result.push_back(hint);
        color = replxx::Replxx::Color::GRAY;
        return result;
      });
  editor_.set_highlighter_callback(
      [completion](const std::string& input, replxx::Replxx::colors_t& colors) {
        const std::size_t end = input.find_first_of(" \t\r\n");
        const std::string head = input.substr(0, end);
        const auto color = completion->IsCommand(head) ? replxx::Replxx::Color::BRIGHTCYAN
                                                       : replxx::Replxx::Color::BRIGHTRED;
        for (std::size_t i = 0; i < std::min(head.size(), colors.size()); ++i) colors[i] = color;
      });

  if (history_enabled_) {
    history_path_ = ResolveHistoryPath();
    std::string warning;
    if (!PrepareHistoryPath(history_path_, &warning)) {
      history_enabled_ = false;
      eos::lib::LogEvent(eos::lib::GetLogger("eos-repl"), spdlog::level::warn,
                         "repl_history_prepare_failed",
                         {{"reason", warning}, {"fallback", "session_history"}});
    } else {
      editor_.history_load(history_path_);
    }
  }
}

Terminal::~Terminal() {
  EndLiveView();
  if (!history_enabled_) return;
  if (!editor_.history_save(history_path_)) {
    eos::lib::LogEvent(eos::lib::GetLogger("eos-repl"), spdlog::level::warn,
                       "repl_history_save_failed", {{"path", history_path_}});
    return;
  }
  std::string warning;
  if (!SecureHistoryFile(history_path_, &warning)) {
    eos::lib::LogEvent(eos::lib::GetLogger("eos-repl"), spdlog::level::warn,
                       "repl_history_secure_failed", {{"reason", warning}});
  }
}

Terminal::ReadStatus Terminal::Read(const std::string& prompt, std::string* line,
                                    const std::string& preload) {
  if (!interactive_) {
    return std::getline(std::cin, *line) ? ReadStatus::kLine : ReadStatus::kEof;
  }
  if (!preload.empty()) editor_.set_preload_buffer(preload);
  errno = 0;
  const char* input = editor_.input(prompt);
  if (input == nullptr) return errno == EAGAIN ? ReadStatus::kInterrupted : ReadStatus::kEof;
  *line = input;
  return ReadStatus::kLine;
}

void Terminal::AddHistory(const std::string& line) {
  if (interactive_ && !line.empty()) editor_.history_add(line);
}

void Terminal::Print(const std::string& text) {
  const std::string output = text + "\n";
  if (interactive_) {
    const std::lock_guard<std::mutex> lock(interactive_output_mutex_);
    editor_.write(output.data(), static_cast<int>(output.size()));
    return;
  }
  const std::lock_guard<std::mutex> lock(plain_output_mutex_);
  std::fwrite(output.data(), 1, output.size(), stdout);
  std::fflush(stdout);
}

void Terminal::BeginLiveView() {
  if (!interactive_) return;
  const std::lock_guard<std::mutex> lock(interactive_output_mutex_);
  if (live_view_) return;
  static constexpr char kEnter[] = "\033[?1049h\033[?25l\033[H\033[2J";
  editor_.write(kEnter, static_cast<int>(sizeof(kEnter) - 1));
  live_view_ = true;
}

void Terminal::Redraw(const std::string& text, const std::string& footer) {
  if (!interactive_) {
    Print(text);
    return;
  }
  const std::lock_guard<std::mutex> lock(interactive_output_mutex_);
  const std::string output = "\033[H\033[J" + text +
                             (footer.empty() ? "\n" : "\n" + footer + "\n");
  editor_.write(output.data(), static_cast<int>(output.size()));
}

void Terminal::EndLiveView() {
  if (!interactive_) return;
  const std::lock_guard<std::mutex> lock(interactive_output_mutex_);
  if (!live_view_) return;
  static constexpr char kLeave[] = "\033[?25h\033[?1049l";
  editor_.write(kLeave, static_cast<int>(sizeof(kLeave) - 1));
  live_view_ = false;
}

bool Terminal::WaitForWatchQuit() {
  if (!interactive_) return false;
  ScopedRawMode raw;
  if (!raw.active()) return false;

  char key = 0;
  while (true) {
    const ssize_t count = ::read(STDIN_FILENO, &key, 1);
    if (count == 1 && key == 'q') break;
    if (count < 0 && errno == EINTR) continue;
    return false;
  }
  return key == 'q';
}

Terminal::WorkflowWatchControl Terminal::WaitForWorkflowWatchControl() {
  if (!interactive_) return WorkflowWatchControl::kHangup;
  ScopedRawMode raw;
  if (!raw.active()) return WorkflowWatchControl::kHangup;

  char key = 0;
  while (true) {
    const ssize_t count = ::read(STDIN_FILENO, &key, 1);
    if (count == 1 && key == 3) return WorkflowWatchControl::kCancel;
    if (count == 1 && key == 'q') return WorkflowWatchControl::kDetach;
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return WorkflowWatchControl::kHangup;
  }
}

Terminal::AttachedControl Terminal::WaitForAttachedControl(
    const std::function<bool()>& terminal, const std::function<void()>& idle) {
  if (!interactive_) return AttachedControl::kHangup;
  ScopedRawMode raw;
  if (!raw.active()) return AttachedControl::kHangup;
  while (!terminal()) {
    pollfd descriptor{STDIN_FILENO, POLLIN, 0};
    const int ready = ::poll(&descriptor, 1, 100);
    if (ready < 0 && errno == EINTR) continue;
    if (ready < 0 || (descriptor.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0)
      return AttachedControl::kHangup;
    if (ready == 0) {
      idle();
      continue;
    }
    char key = 0;
    const ssize_t count = ::read(STDIN_FILENO, &key, 1);
    if (count == 0) return AttachedControl::kHangup;
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) return AttachedControl::kHangup;
    if (key == 3) return AttachedControl::kCancel;
    if (key == 'p') return AttachedControl::kPause;
    if (key == 'r') return AttachedControl::kResume;
    if (key == 's') return AttachedControl::kSignal;
    if (key == 'd' || key == 'q') return AttachedControl::kDetach;
    if (key == '?') return AttachedControl::kHelp;
  }
  return AttachedControl::kTerminal;
}

void Terminal::RunAutoKeys(const std::function<bool(int)>& on_key) {
  if (!interactive_) {
    on_key(-2);
    return;
  }
  ScopedRawMode raw;
  if (!raw.active()) {
    on_key(-2);
    return;
  }
  while (true) {
    pollfd descriptor{STDIN_FILENO, POLLIN, 0};
    const int ready = ::poll(&descriptor, 1, 20);
    if (ready < 0 && errno == EINTR) continue;
    if (ready < 0 || (descriptor.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
      on_key(-2);
      return;
    }
    if (ready == 0) {
      const auto call_started = std::chrono::steady_clock::now();
      const bool keep_running = on_key(-1);
      if (std::chrono::steady_clock::now() - call_started > std::chrono::milliseconds(500))
        ::tcflush(STDIN_FILENO, TCIFLUSH);
      if (!keep_running) return;
      continue;
    }
    char key = 0;
    const ssize_t count = ::read(STDIN_FILENO, &key, 1);
    if (count < 0 && errno == EINTR) continue;
    if (count != 1) {
      on_key(-2);
      return;
    }
    const auto call_started = std::chrono::steady_clock::now();
    const bool keep_running = on_key(static_cast<unsigned char>(key));
    if (std::chrono::steady_clock::now() - call_started > std::chrono::milliseconds(500)) {
      // Discard keys typed while the callback was blocked so stale key
      // events cannot move the base afterwards.
      ::tcflush(STDIN_FILENO, TCIFLUSH);
    }
    if (!keep_running) return;
  }
}

void Terminal::SetPrompt(const std::string& prompt) {
  if (interactive_) editor_.set_prompt(prompt);
}

}  // namespace eos::ext::repl

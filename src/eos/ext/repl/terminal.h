/// TTY-aware line editor and asynchronous output adapter.
#pragma once

#include <functional>
#include <mutex>
#include <replxx.hxx>
#include <string>

#include "completion.h"

namespace eos::ext::repl {

class Terminal final {
 public:
  enum class ReadStatus { kLine, kInterrupted, kEof };
  enum class WorkflowWatchControl { kDetach, kCancel, kHangup };
  enum class AttachedControl {
    kCancel,
    kPause,
    kResume,
    kSignal,
    kDetach,
    kHelp,
    kTerminal,
    kHangup
  };

  Terminal(const CompletionEngine* completion, bool colors, bool history_enabled);
  ~Terminal();

  bool IsInteractive() const { return interactive_; }
  ReadStatus Read(const std::string& prompt, std::string* line, const std::string& preload = {});
  void AddHistory(const std::string& line);
  void Print(const std::string& text);
  /// Enters an alternate-screen viewport for an interactive live display.
  void BeginLiveView();
  /// Replaces the terminal view for an interactive watch update.
  void Redraw(const std::string& text, const std::string& footer = "[q] quit watch");
  /// Restores the terminal after an interactive live display.
  void EndLiveView();
  /// Waits for the single-key quit control used by an interactive watch.
  bool WaitForWatchQuit();
  /// Waits for detach or cooperative workflow cancellation in a targeted watch.
  WorkflowWatchControl WaitForWorkflowWatchControl();
  AttachedControl WaitForAttachedControl(const std::function<bool()>& terminal,
                                         const std::function<void()>& idle);
  // Raw keyboard loop. Callback receives a byte, -1 on idle, or -2 on hangup;
  // returning false exits and restores terminal settings.
  void RunAutoKeys(const std::function<bool(int)>& on_key);
  void SetPrompt(const std::string& prompt);

 private:
  bool interactive_ = false;
  bool colors_ = false;
  bool history_enabled_ = false;
  bool live_view_ = false;
  std::string history_path_;
  replxx::Replxx editor_;
  std::mutex interactive_output_mutex_;
  std::mutex plain_output_mutex_;
};

}  // namespace eos::ext::repl

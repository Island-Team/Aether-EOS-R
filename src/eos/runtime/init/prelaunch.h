/// Deployment-owned startup commands run before EOS constructs hardware adapters.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <sys/types.h>

namespace eos::runtime::init {

struct PrelaunchCommand {
  std::vector<std::string> arguments;
  std::string shell;
  std::string ready_shell;
  bool reuse_if_ready{false};
  /// Supervise a foreground shell service and retry it after exit.
  bool restart_on_exit{false};
};

struct ManagedServiceConfig {
  std::string shell;
  std::string ready_shell;
  std::uint32_t timeout_ms{30000};
  bool reuse_if_ready{false};
};

class PrelaunchProcesses {
 public:
  PrelaunchProcesses() = default;
  PrelaunchProcesses(const PrelaunchProcesses&) = delete;
  PrelaunchProcesses& operator=(const PrelaunchProcesses&) = delete;
  PrelaunchProcesses(PrelaunchProcesses&& other) noexcept;
  PrelaunchProcesses& operator=(PrelaunchProcesses&& other) noexcept;
  ~PrelaunchProcesses();

  void Add(pid_t process);

 private:
  void Stop();
  std::vector<pid_t> processes_;
};

/// Runs argv commands to completion and starts shell services owned by the returned handle.
PrelaunchProcesses RunPrelaunch(const std::vector<PrelaunchCommand>& commands,
                                const std::string& log_directory);

/// Starts a long-running EOS-owned service and waits for its readiness command.
PrelaunchProcesses RunManagedService(const ManagedServiceConfig& config,
                                     const std::string& log_directory);

}  // namespace eos::runtime::init

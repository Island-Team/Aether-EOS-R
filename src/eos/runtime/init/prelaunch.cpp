#include "prelaunch.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "src/eos/lib/log.h"

namespace eos::runtime::init {
namespace {
bool ProbeReady(const std::string& shell, std::chrono::milliseconds timeout);

volatile sig_atomic_t supervisor_stopping = 0;

void StopSupervisor(int) { supervisor_stopping = 1; }

void SuperviseShell(const PrelaunchCommand& command, const std::string& log_path, pid_t owner) {
  setpgid(0, 0);
  prctl(PR_SET_PDEATHSIG, SIGTERM);
  if (getppid() != owner) _exit(0);
  const int output = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (output < 0 || dup2(output, STDOUT_FILENO) < 0 || dup2(output, STDERR_FILENO) < 0)
    _exit(126);
  if (output > STDERR_FILENO) close(output);
  struct sigaction action{};
  action.sa_handler = StopSupervisor;
  sigemptyset(&action.sa_mask);
  sigaction(SIGTERM, &action, nullptr);
  sigaction(SIGINT, &action, nullptr);
  setenv("PYTHONUNBUFFERED", "1", 0);

  bool reported_reuse = false;
  while (!supervisor_stopping) {
    // A reused process is externally owned. Monitor it, and take ownership only
    // after its readiness probe stops succeeding.
    bool already_ready = false;
    if (command.reuse_if_ready) {
      try {
        already_ready = ProbeReady(command.ready_shell, std::chrono::milliseconds(2000));
      } catch (const std::exception& error) {
        dprintf(STDERR_FILENO, "prelaunch supervisor: readiness probe failed: %s\n",
                error.what());
      }
    }
    if (already_ready) {
      if (!reported_reuse) {
        dprintf(STDERR_FILENO, "prelaunch supervisor: reusing ready external service\n");
        reported_reuse = true;
      }
      std::this_thread::sleep_for(std::chrono::seconds(2));
      continue;
    }
    reported_reuse = false;
    if (supervisor_stopping) break;
    const pid_t child = fork();
    if (child < 0) {
      dprintf(STDERR_FILENO, "prelaunch supervisor: fork failed: %s\n", std::strerror(errno));
    } else if (child == 0) {
      signal(SIGTERM, SIG_DFL);
      signal(SIGINT, SIG_DFL);
      execl("/bin/bash", "bash", "-lc", command.shell.c_str(), nullptr);
      _exit(127);
    } else {
      dprintf(STDERR_FILENO, "prelaunch supervisor: started pid=%ld\n", static_cast<long>(child));
      int status = 0;
      bool child_exited = false;
      while (!supervisor_stopping) {
        const pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child) {
          child_exited = true;
          break;
        }
        if (waited < 0 && errno != EINTR) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
      if (supervisor_stopping) {
        if (!child_exited) {
          kill(child, SIGTERM);
          const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
          while (std::chrono::steady_clock::now() < deadline) {
            const pid_t waited = waitpid(child, &status, WNOHANG);
            if (waited == child || (waited < 0 && errno == ECHILD)) {
              child_exited = true;
              break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
          }
          if (!child_exited) {
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
          }
        }
        break;
      }
      dprintf(STDERR_FILENO, "prelaunch supervisor: pid=%ld exited; retrying in 2s\n",
              static_cast<long>(child));
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
  _exit(0);
}

void RecordProcessGroup(pid_t process) {
  const char* path = std::getenv("EOS_PRELAUNCH_PID_FILE");
  if (path == nullptr || *path == '\0') return;
  std::ifstream stat("/proc/" + std::to_string(process) + "/stat");
  std::string line;
  if (!std::getline(stat, line)) return;
  const std::size_t command_end = line.rfind(") ");
  if (command_end == std::string::npos) return;
  std::istringstream fields(line.substr(command_end + 2));
  std::string value;
  for (int field = 3; field <= 22; ++field) {
    if (!(fields >> value)) return;
  }
  std::ofstream registry(path, std::ios::app);
  if (registry) registry << process << ' ' << value << '\n';
}

void WaitFor(pid_t child, const std::string& executable) {
  int status = 0;
  while (waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) {
      throw std::runtime_error("prelaunch wait failed for '" + executable +
                               "': " + std::strerror(errno));
    }
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    throw std::runtime_error("prelaunch command failed: '" + executable + "'");
  }
}

bool ProbeReady(const std::string& shell, std::chrono::milliseconds timeout =
                                             std::chrono::milliseconds(2000)) {
  const pid_t probe = fork();
  if (probe < 0) throw std::runtime_error("readiness probe fork failed");
  if (probe == 0) {
    setpgid(0, 0);
    const int null_output = open("/dev/null", O_WRONLY);
    if (null_output >= 0) {
      dup2(null_output, STDOUT_FILENO);
      dup2(null_output, STDERR_FILENO);
      if (null_output > STDERR_FILENO) close(null_output);
    }
    execl("/bin/bash", "bash", "-lc", shell.c_str(), nullptr);
    _exit(127);
  }
  setpgid(probe, probe);
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  int status = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const pid_t waited = waitpid(probe, &status, WNOHANG);
    if (waited == probe) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (waited < 0 && errno != EINTR)
      throw std::runtime_error("readiness probe wait failed: " + std::string(std::strerror(errno)));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  kill(-probe, SIGKILL);
  while (waitpid(probe, &status, 0) < 0 && errno == EINTR) {}
  return false;
}
}  // namespace

PrelaunchProcesses::PrelaunchProcesses(PrelaunchProcesses&& other) noexcept
    : processes_(std::exchange(other.processes_, {})) {}

PrelaunchProcesses& PrelaunchProcesses::operator=(PrelaunchProcesses&& other) noexcept {
  if (this != &other) {
    Stop();
    processes_ = std::exchange(other.processes_, {});
  }
  return *this;
}

PrelaunchProcesses::~PrelaunchProcesses() { Stop(); }

void PrelaunchProcesses::Add(pid_t process) { processes_.push_back(process); }

void PrelaunchProcesses::Stop() {
  auto logger = lib::GetLogger("eosd");
  for (pid_t process : processes_) {
    if (kill(-process, SIGTERM) != 0 && errno != ESRCH) {
      lib::LogEvent(logger, spdlog::level::warn, "prelaunch_command_status",
                    {{"type", "shell"},
                     {"status", "termination_failed"},
                     {"pid", process},
                     {"reason", std::strerror(errno)}});
    }
  }
  for (pid_t process : processes_) {
    int status = 0;
    pid_t waited = -1;
    do {
      waited = waitpid(process, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited == process) {
      lib::LogEvent(logger, spdlog::level::info, "prelaunch_command_status",
                    {{"type", "shell"},
                     {"status", "stopped"},
                     {"pid", process},
                     {"exit_code", WIFEXITED(status) ? nlohmann::json(WEXITSTATUS(status))
                                                     : nlohmann::json(nullptr)},
                     {"signal", WIFSIGNALED(status) ? nlohmann::json(WTERMSIG(status))
                                                    : nlohmann::json(nullptr)}});
    }
  }
  processes_.clear();
}

PrelaunchProcesses RunPrelaunch(const std::vector<PrelaunchCommand>& commands,
                                const std::string& log_directory) {
  auto logger = lib::GetLogger("eosd");
  PrelaunchProcesses processes;
  std::error_code directory_error;
  std::filesystem::create_directories(log_directory, directory_error);
  if (directory_error) {
    throw std::runtime_error("cannot create prelaunch log directory '" + log_directory +
                             "': " + directory_error.message());
  }
  for (std::size_t index = 0; index < commands.size(); ++index) {
    const PrelaunchCommand& command = commands[index];
    if (command.restart_on_exit) {
      if (command.shell.empty())
        throw std::runtime_error("supervised prelaunch requires shell");
      const std::string log_path =
          log_directory + "/prelaunch-" + std::to_string(index + 1) + ".log";
      const pid_t owner = getpid();
      const pid_t supervisor = fork();
      if (supervisor < 0)
        throw std::runtime_error("prelaunch supervisor fork failed: " +
                                 std::string(std::strerror(errno)));
      if (supervisor == 0) SuperviseShell(command, log_path, owner);
      setpgid(supervisor, supervisor);
      RecordProcessGroup(supervisor);
      processes.Add(supervisor);
      lib::LogEvent(logger, spdlog::level::info, "prelaunch_command_status",
                    {{"index", index + 1},
                     {"type", "shell"},
                     {"status", "supervised"},
                     {"pid", supervisor},
                     {"output", log_path}});
      continue;
    }
    if (command.reuse_if_ready && ProbeReady(command.ready_shell)) {
      lib::LogEvent(logger, spdlog::level::info, "prelaunch_command_status",
                    {{"index", index + 1}, {"type", "shell"}, {"status", "reused"}});
      continue;
    }
    if (!command.shell.empty()) {
      const std::string log_path =
          log_directory + "/prelaunch-" + std::to_string(index + 1) + ".log";
      const pid_t child = fork();
      if (child < 0) {
        throw std::runtime_error("prelaunch fork failed: " + std::string(std::strerror(errno)));
      }
      if (child == 0) {
        setpgid(0, 0);
        setenv("PYTHONUNBUFFERED", "1", 0);
        const int output = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (output < 0 || dup2(output, STDOUT_FILENO) < 0 || dup2(output, STDERR_FILENO) < 0) {
          _exit(126);
        }
        if (output > STDERR_FILENO) close(output);
        execl("/bin/bash", "bash", "-lc", command.shell.c_str(), nullptr);
        _exit(127);
      }
      setpgid(child, child);
      RecordProcessGroup(child);
      processes.Add(child);
      lib::LogEvent(logger, spdlog::level::info, "prelaunch_command_status",
                    {{"index", index + 1},
                     {"type", "shell"},
                     {"status", "running"},
                     {"pid", child},
                     {"output", log_path}});
      continue;
    }
    if (command.arguments.empty()) throw std::runtime_error("prelaunch command must not be empty");
    const pid_t child = fork();
    if (child < 0) {
      throw std::runtime_error("prelaunch fork failed for '" + command.arguments.front() +
                               "': " + std::strerror(errno));
    }
    if (child == 0) {
      std::vector<char*> arguments;
      arguments.reserve(command.arguments.size() + 1);
      for (const std::string& argument : command.arguments) {
        arguments.push_back(const_cast<char*>(argument.c_str()));
      }
      arguments.push_back(nullptr);
      execvp(arguments.front(), arguments.data());
      _exit(127);
    }

    WaitFor(child, command.arguments.front());
    lib::LogEvent(logger, spdlog::level::info, "prelaunch_command_status",
                  {{"index", index + 1},
                   {"type", "command"},
                   {"executable", command.arguments.front()},
                   {"status", "completed"}});
  }
  return processes;
}

PrelaunchProcesses RunManagedService(const ManagedServiceConfig& config,
                                     const std::string& log_directory) {
  if (config.shell.empty() || config.ready_shell.empty()) {
    throw std::runtime_error("managed service requires shell and ready_shell");
  }
  PrelaunchProcesses processes;
  if (config.reuse_if_ready &&
      ProbeReady(config.ready_shell, std::chrono::milliseconds(5000))) {
    lib::LogEvent(lib::GetLogger("eosd"), spdlog::level::info, "managed_adapter_ready",
                  {{"status", "reused"}});
    return processes;
  }
  const std::string log_path = log_directory + "/adapter.log";
  const pid_t child = fork();
  if (child < 0) throw std::runtime_error("managed service fork failed: " + std::string(std::strerror(errno)));
  if (child == 0) {
    setpgid(0, 0);
    const int output = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (output < 0 || dup2(output, STDOUT_FILENO) < 0 || dup2(output, STDERR_FILENO) < 0) _exit(126);
    if (output > STDERR_FILENO) close(output);
    execl("/bin/bash", "bash", "-lc", config.shell.c_str(), nullptr);
    _exit(127);
  }
  setpgid(child, child);
  RecordProcessGroup(child);
  processes.Add(child);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(config.timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    int status = 0;
    const pid_t exited = waitpid(child, &status, WNOHANG);
    if (exited == child) {
      throw std::runtime_error("managed adapter exited before readiness (see " + log_path + ")");
    }
    const pid_t probe = fork();
    if (probe < 0) throw std::runtime_error("managed adapter readiness fork failed");
    if (probe == 0) {
      const int null_output = open("/dev/null", O_WRONLY);
      if (null_output >= 0) {
        dup2(null_output, STDOUT_FILENO);
        dup2(null_output, STDERR_FILENO);
        if (null_output > STDERR_FILENO) close(null_output);
      }
      execl("/bin/bash", "bash", "-lc", config.ready_shell.c_str(), nullptr);
      _exit(127);
    }
    int probe_status = 0;
    pid_t probe_waited = -1;
    do {
      probe_waited = waitpid(probe, &probe_status, 0);
    } while (probe_waited < 0 && errno == EINTR);
    if (probe_waited == probe && WIFEXITED(probe_status) && WEXITSTATUS(probe_status) == 0) {
      // A pre-existing service can satisfy the probe while this child exits. Give
      // short-lived launch failures time to surface before declaring ownership.
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      const pid_t settled = waitpid(child, &status, WNOHANG);
      if (settled == child) {
        throw std::runtime_error("managed adapter exited after readiness (see " + log_path + ")");
      }
      if (settled < 0) {
        throw std::runtime_error("managed adapter wait failed: " + std::string(std::strerror(errno)));
      }
      lib::LogEvent(lib::GetLogger("eosd"), spdlog::level::info, "managed_adapter_ready",
                    {{"pid", child}, {"output", log_path}});
      return processes;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  throw std::runtime_error("managed adapter readiness timed out (see " + log_path + ")");
}
}  // namespace eos::runtime::init

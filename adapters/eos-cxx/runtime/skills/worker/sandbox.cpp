#include "adapters/eos-cxx/runtime/skills/worker/sandbox.h"

#include <fcntl.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace eos::runtime::skills::worker {
namespace {
void Check(bool ok, const char* operation) {
  if (!ok) throw std::runtime_error(std::string("skill sandbox: ") + operation);
}

void Limit(int resource, rlim_t soft, rlim_t hard) {
  rlimit limit{soft, hard};
  Check(setrlimit(resource, &limit) == 0, "resource limit unavailable");
}

void AllowRead(int ruleset, const std::filesystem::path& path) {
  const int fd = open(path.c_str(), O_PATH | O_CLOEXEC);
  Check(fd >= 0, "allowed path unavailable");
  landlock_path_beneath_attr rule{};
  rule.allowed_access = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR |
                        LANDLOCK_ACCESS_FS_EXECUTE;
  rule.parent_fd = fd;
  const int result = static_cast<int>(syscall(SYS_landlock_add_rule, ruleset,
      LANDLOCK_RULE_PATH_BENEATH, &rule, 0));
  close(fd);
  Check(result == 0, "Landlock path rule unavailable");
}

void RestrictFilesystem(const std::filesystem::path& directory) {
  landlock_ruleset_attr rules{};
  rules.handled_access_fs = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE |
      LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR |
      LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
      LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR |
      LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK |
      LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
      LANDLOCK_ACCESS_FS_MAKE_SYM;
  const int ruleset = static_cast<int>(syscall(SYS_landlock_create_ruleset,
      &rules, sizeof(rules), 0));
  Check(ruleset >= 0, "Landlock unavailable");
  try {
    for (const char* path : {"/usr", "/lib", "/lib64", "/etc"})
      if (std::filesystem::exists(path)) AllowRead(ruleset, path);
    AllowRead(ruleset, std::filesystem::canonical(directory));
    Check(syscall(SYS_landlock_restrict_self, ruleset, 0) == 0,
          "Landlock enforcement unavailable");
  } catch (...) { close(ruleset); throw; }
  close(ruleset);
}

void RestrictSyscalls() {
  // Keep the already connected broker socket usable. New network endpoints,
  // process creation and replacement are unavailable to loaded skill code.
#define DENY(nr) \
  BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, nr, 0, 1), \
  BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM)
  sock_filter code[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
      // Reject x32 syscall numbers before comparing the native table.
      BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000U, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS),
      DENY(__NR_socket), DENY(__NR_socketpair), DENY(__NR_connect),
      DENY(__NR_bind), DENY(__NR_listen), DENY(__NR_accept),
      DENY(__NR_accept4), DENY(__NR_fork), DENY(__NR_vfork),
      DENY(__NR_execve), DENY(__NR_execveat),
      DENY(__NR_io_uring_setup), DENY(__NR_io_uring_enter),
      DENY(__NR_ptrace), DENY(__NR_process_vm_readv),
      DENY(__NR_process_vm_writev), DENY(__NR_pidfd_open),
      DENY(__NR_pidfd_getfd), DENY(__NR_kill), DENY(__NR_tkill),
      DENY(__NR_tgkill), DENY(__NR_bpf), DENY(__NR_perf_event_open),
      DENY(__NR_mount), DENY(__NR_umount2), DENY(__NR_unshare),
      DENY(__NR_setns), DENY(__NR_chroot),
      // glibc retries legacy clone when clone3 reports ENOSYS.
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_clone3, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS),
      // clone is needed for skill-local threads, but a child process would
      // inherit the broker descriptor and could outlive the worker session.
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_clone, 0, 4),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args[0])),
      BPF_STMT(BPF_ALU | BPF_AND | BPF_K, CLONE_THREAD),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, CLONE_THREAD, 1, 0),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
#undef DENY
  sock_fprog program{static_cast<unsigned short>(sizeof(code) / sizeof(code[0])), code};
  Check(prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0,
        "seccomp enforcement unavailable");
}
}  // namespace

void ApplySandbox(const std::filesystem::path& skill_directory) {
  Check(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0,
        "no_new_privs unavailable");
  Limit(RLIMIT_CORE, 0, 0);
  Limit(RLIMIT_FSIZE, 0, 0);
  Limit(RLIMIT_NOFILE, 64, 64);
  RestrictFilesystem(skill_directory);
  RestrictSyscalls();
}
}  // namespace eos::runtime::skills::worker

#include "adapters/eos-cxx/runtime/skills/worker/sandbox.h"

#include <gtest/gtest.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace eos::runtime::skills::worker {
namespace {
TEST(SkillSandbox, AllowsSkillReadAndDeniesWritesAndNewSockets) {
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    try {
      ApplySandbox(EOS_TEST_SKILL_V2_DIR);
      const int allowed = open(EOS_TEST_SKILL_V2_DIR, O_RDONLY | O_DIRECTORY);
      const int denied = open("/tmp/eos-skill-sandbox-test", O_CREAT | O_WRONLY, 0600);
      const int network = socket(AF_INET, SOCK_STREAM, 0);
      const pid_t spawned = fork();
      if (spawned == 0) _exit(3);
      if (allowed >= 0) close(allowed);
      if (denied >= 0) close(denied);
      if (network >= 0) close(network);
      _exit(allowed >= 0 && denied < 0 && network < 0 && spawned < 0 ? 0 : 1);
    } catch (...) { _exit(2); }
  }
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}
}  // namespace
}  // namespace eos::runtime::skills::worker

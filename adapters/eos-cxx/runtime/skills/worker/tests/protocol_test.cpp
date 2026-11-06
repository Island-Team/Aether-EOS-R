#include "adapters/eos-cxx/runtime/skills/worker/protocol.h"

#include <fcntl.h>
#include <linux/memfd.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <string>

namespace eos::runtime::skills::worker {
namespace {
struct Pair {
  std::array<int, 2> fd{};
  Pair() { EXPECT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fd.data()), 0); }
  ~Pair() { close(fd[0]); close(fd[1]); }
};

TEST(WorkerProtocol, RoundTripsBoundedPacketAndCredentials) {
  Pair pair;
  const std::string payload(kMaxPayloadBytes, 'x');
  Send(pair.fd[0], {MessageType::kExecute, 42, 7, payload});
  auto received = Receive(pair.fd[1]);
  ASSERT_TRUE(received);
  EXPECT_EQ(received->type, MessageType::kExecute);
  EXPECT_EQ(received->request_id, 42u);
  EXPECT_EQ(received->generation, 7u);
  EXPECT_EQ(received->payload, payload);
  EXPECT_EQ(received->payload_fd, -1);
  EXPECT_EQ(Credentials(pair.fd[1]).uid, getuid());
  EXPECT_THROW(Send(pair.fd[0], {MessageType::kExecute, 1, 0,
                                std::string(kMaxPayloadBytes + 1, 'x')}), std::runtime_error);
}

TEST(WorkerProtocol, RejectsTruncatedAndIncompatiblePackets) {
  Pair pair;
  const char tiny[] = "bad";
  ASSERT_EQ(send(pair.fd[0], tiny, sizeof(tiny), 0), sizeof(tiny));
  EXPECT_THROW(Receive(pair.fd[1]), std::runtime_error);
  Send(pair.fd[0], {MessageType::kHello, 1, 0, "ok"});
  EXPECT_EQ(Receive(pair.fd[1])->payload, "ok");
  std::string oversized(kMaxPayloadBytes + 100, 'a');
  ASSERT_EQ(send(pair.fd[0], oversized.data(), oversized.size(), 0), oversized.size());
  EXPECT_THROW(Receive(pair.fd[1]), std::runtime_error);
}

TEST(WorkerProtocol, AcceptsOnlySealedMemfd) {
  Pair pair;
  const int fd = static_cast<int>(syscall(SYS_memfd_create, "skill-payload", MFD_ALLOW_SEALING));
  ASSERT_GE(fd, 0);
  EXPECT_THROW(Send(pair.fd[0], {MessageType::kResult, 3, 2, "", fd}), std::runtime_error);
  ASSERT_EQ(fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK), 0);
  Send(pair.fd[0], {MessageType::kResult, 3, 2, "", fd});
  auto received = Receive(pair.fd[1]);
  ASSERT_TRUE(received);
  ASSERT_GE(received->payload_fd, 0);
  EXPECT_NE(received->payload_fd, fd);
  EXPECT_NE(fcntl(received->payload_fd, F_GET_SEALS) & F_SEAL_WRITE, 0);
  close(received->payload_fd);
  close(fd);
}

TEST(WorkerProtocol, CleanShutdownIsDistinctFromMalformedPacket) {
  Pair pair;
  close(pair.fd[0]);
  pair.fd[0] = -1;
  EXPECT_FALSE(Receive(pair.fd[1]));
}
}  // namespace
}  // namespace eos::runtime::skills::worker

#include "adapters/eos-cxx/runtime/skills/worker/grants.h"

namespace eos::runtime::skills::worker {
namespace {
TEST(WorkerGrants, MatchExactExecutionGenerationPrimitiveAndDevice) {
  GrantTable table;
  const auto expiry = std::chrono::steady_clock::now() + std::chrono::minutes(1);
  const std::string token = table.Issue({"exec-1", 7, "joint.move", "1.0", "arm.left", expiry});
  CapabilityCall call{token, "exec-1", 7, "joint.move", "1.0", "arm.left"};
  EXPECT_TRUE(table.Authorize(call));
  call.device_id = "arm.right";
  EXPECT_FALSE(table.Authorize(call));
  call.device_id = "arm.left";
  call.generation = 8;
  EXPECT_FALSE(table.Authorize(call));
  call.generation = 7;
  call.primitive = "joint.stop";
  EXPECT_FALSE(table.Authorize(call));
  call.primitive = "joint.move";
  table.RevokeExecution("exec-1");
  EXPECT_FALSE(table.Authorize(call));
}
TEST(WorkerGrants, RejectsExpiredAndRetiredGeneration) {
  GrantTable table;
  EXPECT_THROW(table.Issue({"exec-1", 1, "move", "1", "arm",
                            std::chrono::steady_clock::now() - std::chrono::seconds(1)}),
               std::invalid_argument);
  const std::string token = table.Issue({"exec-1", 1, "move", "1", "arm",
                                        std::chrono::steady_clock::now() + std::chrono::minutes(1)});
  CapabilityCall call{token, "exec-1", 1, "move", "1", "arm"};
  table.RevokeGeneration(1);
  EXPECT_FALSE(table.Authorize(call));
}
}  // namespace
}  // namespace eos::runtime::skills::worker

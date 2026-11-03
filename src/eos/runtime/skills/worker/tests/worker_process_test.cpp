#include "src/eos/runtime/skills/worker/protocol.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>

namespace eos::runtime::skills::worker {
namespace {
TEST(SkillWorkerProcess, ExecutesNativeFixtureAcrossProcessBoundary) {
  int sockets[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets), 0);
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    close(sockets[0]);
    if (dup2(sockets[1], 3) < 0) _exit(125);
    close(sockets[1]);
    execl(EOS_SKILL_WORKER_EXE, EOS_SKILL_WORKER_EXE, nullptr);
    _exit(126);
  }
  close(sockets[1]);
  const nlohmann::json hello = {
      {"directory", EOS_TEST_SKILL_V2_DIR},
      {"primitives", nlohmann::json::array({{{"name", "base.posture"},
                                              {"version", "1.0.0"}}})}};
  Send(sockets[0], {MessageType::kHello, 11, 0, hello.dump()});
  auto catalog = Receive(sockets[0]);
  ASSERT_TRUE(catalog);
  EXPECT_EQ(catalog->type, MessageType::kCatalog);
  const auto skills = nlohmann::json::parse(catalog->payload);
  ASSERT_FALSE(skills.empty());
  const nlohmann::json request = {{"name", "fixture.signals"},
                                  {"version", "1.0.0"},
                                  {"arguments", nlohmann::json::object()},
                                  {"command_id", "worker-process-test"}};
  Send(sockets[0], {MessageType::kExecute, 12, 4, request.dump()});
  auto result = Receive(sockets[0]);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->type, MessageType::kResult);
  EXPECT_EQ(result->generation, 4u);
  const auto outcome = nlohmann::json::parse(result->payload);
  EXPECT_TRUE(outcome.at("accepted").get<bool>()) << outcome.dump();
  Send(sockets[0], {MessageType::kStop, 13, 4, ""});
  close(sockets[0]);
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(SkillWorkerProcess, CommunityCallbacksUseExistingBrokerTransport) {
  int sockets[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets), 0);
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    close(sockets[0]);
    if (dup2(sockets[1], 3) < 0) _exit(125);
    close(sockets[1]);
    execl(EOS_SKILL_WORKER_EXE, EOS_SKILL_WORKER_EXE, nullptr);
    _exit(126);
  }
  close(sockets[1]);
  const nlohmann::json hello = {{"directory", EOS_TEST_SKILL_V2_DIR},
      {"primitives", nlohmann::json::array({{{"name", "base.posture"},
                                              {"version", "1.0.0"}}})}};
  Send(sockets[0], {MessageType::kHello, 21, 0, hello.dump()});
  ASSERT_TRUE(Receive(sockets[0]));
  const nlohmann::json request = {{"name", "fixture.community"}, {"version", "1.0.0"},
                                  {"arguments", nlohmann::json::object()},
                                  {"command_id", "worker-community-test"},
                                  {"execution_id", "execution-community"}};
  Send(sockets[0], {MessageType::kExecute, 22, 5, request.dump()});
  auto sent = Receive(sockets[0]);
  ASSERT_TRUE(sent);
  ASSERT_EQ(sent->type, MessageType::kCapabilityCall);
  const auto send_call = nlohmann::json::parse(sent->payload);
  EXPECT_EQ(send_call.at("kind"), "community.send");
  EXPECT_EQ(send_call.at("execution_id"), "execution-community");
  Send(sockets[0], {MessageType::kCapabilityReply, sent->request_id, 5,
                    R"({"ok":true,"sent":true})"});
  auto received = Receive(sockets[0]);
  ASSERT_TRUE(received);
  ASSERT_EQ(received->type, MessageType::kCapabilityCall);
  EXPECT_EQ(nlohmann::json::parse(received->payload).at("kind"), "community.receive");
  const nlohmann::json reply = {{"ok", true}, {"value", {{"message_id", "m"},
      {"request_id", "r"}, {"correlation_id", "r"},
      {"sender_execution_id", "peer"}, {"channel", "process.test"},
      {"topic_message", false}, {"sequence", 1}, {"dropped_before", 0},
      {"payload", {{"value", "world"}}}}}};
  Send(sockets[0], {MessageType::kCapabilityReply, received->request_id, 5, reply.dump()});
  auto result = Receive(sockets[0]);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->type, MessageType::kResult);
  EXPECT_TRUE(nlohmann::json::parse(result->payload).at("accepted").get<bool>());
  Send(sockets[0], {MessageType::kStop, 23, 5, ""});
  close(sockets[0]);
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}
}  // namespace
}  // namespace eos::runtime::skills::worker

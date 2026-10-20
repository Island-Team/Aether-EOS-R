#include "src/eos/ext/north_http/client/client.h"

#include <gtest/gtest.h>

namespace eos::ext::north_http {
namespace {

TEST(NorthHttpClientTest, OwnsOneStableChannelAndAllServiceStubs) {
  Client client("127.0.0.1:1");

  ASSERT_NE(client.channel(), nullptr);
  const grpc::Channel* const channel = client.channel().get();
  auto* const gateway = &client.gateway();
  auto* const skills = &client.skills();
  auto* const workflows = &client.workflows();
  EXPECT_EQ(client.channel().get(), channel);
  EXPECT_EQ(&client.gateway(), gateway);
  EXPECT_EQ(&client.skills(), skills);
  EXPECT_EQ(&client.workflows(), workflows);
}

}  // namespace
}  // namespace eos::ext::north_http

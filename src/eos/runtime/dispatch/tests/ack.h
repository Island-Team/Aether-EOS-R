#pragma once
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

#include "src/eos/runtime/hardware/registry.h"
#include "src/eos/runtime/dispatch/dispatcher.h"
#include "src/eos/runtime/init/schemas.h"

namespace dispatch_test {
inline std::string VersionOf(const std::string& primitive) {
  return primitive == "arm.movej" || primitive == "arm.movel" || primitive == "speaker.play"
             ? "1.0.0"
             : "1.0.0";
}
inline const eos::runtime::init::SchemaStore& Store() {
  static const eos::runtime::init::SchemaStore store(EOS_CONTRACTS_ROOT);
  return store;
}
inline eos::runtime::hardware_runtime::Registry DemoRobot() {
  return eos::runtime::hardware_runtime::Registry::Load(std::string(EOS_SOURCE_ETC) + "/backends/demo.toml",
                                              "demo");
}
/// Test-only derivative of the committed demo binding that adds base velocity.
inline eos::runtime::hardware_runtime::Registry DemoRobotWithBaseCapabilities(bool velocity) {
  std::ifstream input(std::string(EOS_SOURCE_ETC) + "/backends/demo.toml");
  std::ostringstream contents;
  contents << input.rdbuf();
  std::string binding = contents.str();
  if (velocity) {
    const std::string marker = "[profile.demo.device.backend]\n";
    const std::string primitive = R"toml([[profile.demo.device.primitive]]
name = "base.velocity"
version = "1.0.0"
description = "Test-only leased body-frame velocity."
limits = { linear_x_m_s = { minimum = -1.0, maximum = 1.0 }, linear_y_m_s = { minimum = -1.0, maximum = 1.0 }, angular_z_rad_s = { minimum = -1.0, maximum = 1.0 }, valid_for_ms = { minimum = 1.0, maximum = 1000.0 } }

)toml";
    binding.insert(binding.find(marker), primitive);
  }
  return eos::runtime::hardware_runtime::Registry::Adopt(
      eos::runtime::hardware_runtime::ParseBinding(binding, "dispatch-demo-fixture.toml", "demo"));
}
inline nlohmann::json Exec(eos::runtime::dispatch::Dispatcher& dispatcher,
                           const std::string& primitive, const std::string& device_id,
                           const nlohmann::json& arguments = nlohmann::json::object()) {
  const auto result = dispatcher.Execute(primitive, VersionOf(primitive), device_id, arguments);
  EXPECT_EQ(result.status, eos::runtime::dispatch::DispatchResult::Status::kCompleted)
      << result.message;
  return result.payload;
}
inline void ExpectAccepted(const nlohmann::json& ack, const std::string& primitive) {
  EXPECT_EQ(ack.at("type"), "primitive.ack");
  EXPECT_EQ(ack.at("primitive"), primitive);
  EXPECT_EQ(ack.at("primitive_version"), VersionOf(primitive));
  EXPECT_TRUE(ack.at("accepted").get<bool>());
  EXPECT_TRUE(ack.at("reason").is_null());
  EXPECT_TRUE(ack.at("reason_code").is_null());
  EXPECT_NO_THROW(Store().ValidateAck(ack));
}
inline void ExpectRejected(const nlohmann::json& ack, const std::string& primitive,
                           const char* reason_code, const char* version = nullptr) {
  EXPECT_EQ(ack.at("type"), "primitive.ack");
  EXPECT_EQ(ack.at("primitive"), primitive);
  EXPECT_EQ(ack.at("primitive_version"), version == nullptr ? VersionOf(primitive) : version);
  EXPECT_FALSE(ack.at("accepted").get<bool>());
  EXPECT_EQ(ack.at("reason_code"), reason_code);
  EXPECT_NO_THROW(Store().ValidateAck(ack));
}
inline void ExpectInvalid(eos::runtime::dispatch::Dispatcher& dispatcher,
                          const std::string& primitive, const std::string& device_id,
                          const nlohmann::json& arguments) {
  const auto result = dispatcher.Execute(primitive, VersionOf(primitive), device_id, arguments);
  if (result.status == eos::runtime::dispatch::DispatchResult::Status::kCompleted) {
    ExpectRejected(result.payload, primitive, "rejected.invalid_arguments");
  } else {
    EXPECT_EQ(result.status, eos::runtime::dispatch::DispatchResult::Status::kInvalidArgument)
        << result.message;
  }
}
}  // namespace dispatch_test

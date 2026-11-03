#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "reader.h"
#include "src/eos/lib/clock.h"
#include "src/eos/runtime/hardware/catalog.h"
#include "src/eos/runtime/hardware/registry.h"

namespace {
namespace hardware = eos::hardware;
namespace runtime = eos::runtime::hardware_runtime;
using eos::runtime::perception::body::Reader;

struct Source {
  hardware::ObstacleSnapshot snapshot;
  bool fail{false};
};

class FakeBase final : public hardware::Backend {
 public:
  FakeBase(std::string id, std::shared_ptr<Source> source, bool configured)
      : id_(std::move(id)), source_(std::move(source)), configured_(configured) {}
  hardware::BackendIdentity Identify() const override {
    return {.name = "obstacle-test", .version = "1.0.0", .device_id = id_,
            .kind = hardware::DeviceKind::kBase};
  }
  hardware::FeedDeclarations DeclareFeeds() const override { return {}; }
  hardware::BackendHealth ReadHealth() const override {
    return {.alive = true, .admitted = IsAdmitted()};
  }
  bool SupportsObstacleSnapshot() const override { return configured_; }
  std::optional<hardware::ObstacleSnapshot> ReadObstacleSnapshot() const override {
    if (source_->fail) throw std::runtime_error("collector unavailable");
    return source_->snapshot;
  }
  void Clamp() override {}
  void Halt() override {}

 private:
  std::string id_;
  std::shared_ptr<Source> source_;
  bool configured_;
};

runtime::Registry MakeRegistry(std::shared_ptr<Source> source, bool configured) {
  runtime::BackendCatalog catalog;
  runtime::BackendDescriptor descriptor;
  descriptor.type = "obstacle-test";
  descriptor.supports_class = [](std::string_view kind) { return kind == "base"; };
  descriptor.create = [source, configured](std::string id, const runtime::PartConfig&,
                                           const runtime::BackendSettings&) {
    return std::make_unique<FakeBase>(std::move(id), source, configured);
  };
  catalog.Register(std::move(descriptor));
  runtime::Binding binding;
  binding.path = "obstacle-test";
  binding.profile = "test";
  binding.robot.id = "test-id";
  binding.robot.name = "sample-robot";
  runtime::DeviceBinding device;
  device.backend_type = "obstacle-test";
  device.part.kind = "base";
  device.backend_settings = std::make_shared<runtime::BackendSettings>();
  binding.devices.push_back(std::move(device));
  return runtime::Registry::Adopt(catalog, std::move(binding));
}

TEST(BaseObstaclesTest, AdvertisesOnlyConfiguredBaseAndProjectsFreshSectors) {
  auto source = std::make_shared<Source>();
  source->snapshot.state = hardware::ObstacleState::kFresh;
  source->snapshot.age_s = 0.125;
  source->snapshot.source_stamp_s = 123.0;
  source->snapshot.sectors[0] = {2, 0.7};
  auto registry = MakeRegistry(source, true);
  eos::lib::ManualClock clock;
  Reader reader(registry, clock);
  const std::string uri = "robots/sample-robot/base/obstacles/state";
  EXPECT_TRUE(reader.SupportsResourceTemplate("robots/{robot_name}/base/obstacles/state"));
  EXPECT_NE(reader.ListResources().dump().find(uri), std::string::npos);
  const auto value = reader.BuildByUri(uri);
  EXPECT_EQ(value.at("type"), "base.obstacles");
  EXPECT_EQ(value.at("frame_id"), "body");
  EXPECT_EQ(value.at("state"), "fresh");
  EXPECT_EQ(value.at("age_s"), 0.125);
  EXPECT_EQ(value.at("source_stamp_s"), 123.0);
  EXPECT_EQ(value.at("sectors").size(), 8u);
  EXPECT_EQ(value.at("sectors").at("front").at("point_count"), 2);
  EXPECT_EQ(value.at("sectors").at("front").at("distance_m"), 0.7);
  EXPECT_EQ(value.at("sectors").at("front_left").at("point_count"), 0);
  EXPECT_FALSE(value.at("sectors").at("front_left").contains("distance_m"));

  auto without = MakeRegistry(source, false);
  Reader other(without, clock);
  EXPECT_FALSE(other.SupportsResourceTemplate("robots/{robot_name}/base/obstacles/state"));
  EXPECT_EQ(other.ListResources().dump().find(uri), std::string::npos);
  EXPECT_THROW(other.BuildByUri(uri), std::runtime_error);
}

TEST(BaseObstaclesTest, StaleAndTransportFailureNeverExposeDistances) {
  auto source = std::make_shared<Source>();
  auto registry = MakeRegistry(source, true);
  eos::lib::ManualClock clock;
  Reader reader(registry, clock);
  const std::string uri = "robots/sample-robot/base/obstacles/state";
  source->snapshot.sectors[0] = {9, 0.2};
  for (const auto [state, label] :
       {std::pair{hardware::ObstacleState::kUnavailable, "unavailable"},
        std::pair{hardware::ObstacleState::kStale, "stale"},
        std::pair{hardware::ObstacleState::kSourceError, "source_error"}}) {
    source->snapshot.state = state;
    const auto unknown = reader.BuildByUri(uri);
    EXPECT_EQ(unknown.at("state"), label);
    EXPECT_EQ(unknown.at("sectors").at("front").at("point_count"), 0);
    EXPECT_FALSE(unknown.at("sectors").at("front").contains("distance_m"));
  }
  source->fail = true;
  const auto failed = reader.BuildByUri(uri);
  EXPECT_EQ(failed.at("state"), "transport_error");
  EXPECT_EQ(failed.at("sectors").at("front").at("point_count"), 0);
}

TEST(BaseObstaclesTest, RepeatedReadsExposeChangesForWatchSampling) {
  auto source = std::make_shared<Source>();
  auto registry = MakeRegistry(source, true);
  eos::lib::ManualClock clock;
  Reader reader(registry, clock);
  const std::string uri = "robots/sample-robot/base/obstacles/state";
  const auto before = reader.BuildByUri(uri, false);
  source->snapshot.state = hardware::ObstacleState::kFresh;
  source->snapshot.sectors[7] = {1, 1.25};
  const auto after = reader.BuildByUri(uri, false);
  EXPECT_EQ(before.at("state"), "unavailable");
  EXPECT_EQ(after.at("state"), "fresh");
  EXPECT_EQ(after.at("sequence"), before.at("sequence").get<std::uint64_t>() + 1);
  EXPECT_EQ(after.at("sectors").at("front_right").at("distance_m"), 1.25);
}
}  // namespace

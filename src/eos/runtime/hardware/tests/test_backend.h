#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "src/eos/runtime/hardware/catalog.h"

namespace eos::runtime::hardware_runtime::test {

inline hardware::DeviceKind KindOf(std::string_view kind) {
  if (kind == "base") return hardware::DeviceKind::kBase;
  if (kind == "arm") return hardware::DeviceKind::kManipulator;
  if (kind == "gimbal") return hardware::DeviceKind::kGimbal;
  if (kind == "cam") return hardware::DeviceKind::kCamera;
  if (kind == "mic") return hardware::DeviceKind::kMicrophone;
  if (kind == "gripper") return hardware::DeviceKind::kGripper;
  return hardware::DeviceKind::kSpeaker;
}

class TestBackend final : public hardware::Backend {
 public:
  TestBackend(std::string id, std::string kind) : id_(std::move(id)), kind_(std::move(kind)) {}
  hardware::BackendIdentity Identify() const override {
    return {.name = "test", .version = "1.0.0", .device_id = id_, .kind = KindOf(kind_)};
  }
  hardware::FeedDeclarations DeclareFeeds() const override {
    hardware::FeedDeclarations feeds;
    feeds.health.max_rate_hz = 10.0;
    feeds.proprioception.max_rate_hz = 10.0;
    return feeds;
  }
  hardware::BackendHealth ReadHealth() const override {
    return {.alive = alive_, .admitted = IsAdmitted()};
  }
  void Clamp() override {}
  void Halt() override { alive_ = false; }

 private:
  std::string id_;
  std::string kind_;
  bool alive_{true};
};

inline BackendCatalog Catalog(bool compiled = true, bool base_only = false,
                              bool wrong_identity = false) {
  BackendCatalog catalog;
  BackendDescriptor descriptor;
  descriptor.type = "test";
  descriptor.compiled = compiled;
  descriptor.supports_class = [base_only](std::string_view kind) {
    return !base_only || kind == "base";
  };
  descriptor.parse = [](const toml::table& table, const PartConfig&, const std::string& path,
                        const std::string&) -> std::shared_ptr<const BackendSettings> {
    if (table.contains("test")) throw std::runtime_error(path + ": test backend rejected settings");
    return std::make_shared<BackendSettings>();
  };
  descriptor.supports_primitive = [](const PrimitiveSpec&) { return true; };
  descriptor.create = [wrong_identity](
                          std::string id, const PartConfig& part,
                          const BackendSettings&) -> std::unique_ptr<hardware::Backend> {
    return std::make_unique<TestBackend>(wrong_identity ? "wrong-id" : std::move(id), part.kind);
  };
  catalog.Register(std::move(descriptor));
  return catalog;
}

}  // namespace eos::runtime::hardware_runtime::test

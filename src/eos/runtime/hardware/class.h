#pragma once

#include <memory>
#include <string>
#include <toml++/toml.hpp>

#include "registry.h"

namespace eos::ext::simulator::ipc {
class SimulatorClient;
}

namespace eos::runtime::hardware_runtime::detail {
[[noreturn]] void Fail(const std::string& path, const std::string& what);
double OptionalLength(const toml::table& table, const std::string& key, double fallback,
                      const std::string& path, const std::string& where);
double OptionalCoordinate(const toml::table& table, const std::string& key, double fallback,
                          const std::string& path, const std::string& where);
void RequireNoPartKeys(const toml::table& table, const PartConfig& config, const std::string& path,
                       const std::string& where);

void ApplyArmDefaults(PartConfig& config);
void ParseArmPart(const toml::table& table, PartConfig& config, const std::string& path,
                  const std::string& where);
void ValidateArm(const DeviceBinding& device, const std::string& path, const std::string& where);
std::unique_ptr<hardware::Backend> MakeArmBackend(
    const std::string& type, std::string device_id, const PartConfig& part,
    std::shared_ptr<ext::simulator::ipc::SimulatorClient> simulator_world);

void ApplyBaseDefaults(PartConfig& config);
void ParseBasePart(const toml::table& table, PartConfig& config, const std::string& path,
                   const std::string& where);
std::unique_ptr<hardware::Backend> MakeBaseBackend(
    const std::string& type, std::string device_id, const PartConfig& part,
    std::shared_ptr<ext::simulator::ipc::SimulatorClient> simulator_world);

void ApplyCameraDefaults(PartConfig& config);
void ParseCameraPart(const toml::table& table, PartConfig& config, const std::string& path,
                     const std::string& where);
std::unique_ptr<hardware::Backend> MakeCameraBackend(
    const std::string& type, std::string device_id, const PartConfig& part,
    std::shared_ptr<ext::simulator::ipc::SimulatorClient> simulator_world);

void ApplyGimbalDefaults(PartConfig& config);
void ParseGimbalPart(const toml::table& table, PartConfig& config, const std::string& path,
                     const std::string& where);
std::unique_ptr<hardware::Backend> MakeGimbalBackend(
    const std::string& type, std::string device_id, const PartConfig& part,
    std::shared_ptr<ext::simulator::ipc::SimulatorClient> simulator_world);

void ParseGripperPart(const toml::table& table, PartConfig& config, const std::string& path,
                      const std::string& where);
std::unique_ptr<hardware::Backend> MakeGripperBackend(
    const std::string& type, std::string device_id, const PartConfig& part,
    std::shared_ptr<ext::simulator::ipc::SimulatorClient> simulator_world);

void ParseMicPart(const toml::table& table, PartConfig& config, const std::string& path,
                  const std::string& where);
std::unique_ptr<hardware::Backend> MakeMicBackend(
    const std::string& type, std::string device_id, const PartConfig& part,
    std::shared_ptr<ext::simulator::ipc::SimulatorClient> simulator_world);

void ParseSpeakerPart(const toml::table& table, PartConfig& config, const std::string& path,
                      const std::string& where);
std::unique_ptr<hardware::Backend> MakeSpeakerBackend(
    const std::string& type, std::string device_id, const PartConfig& part,
    std::shared_ptr<ext::simulator::ipc::SimulatorClient> simulator_world);

std::unique_ptr<hardware::Backend> MakeBackend(
    const std::string& type, std::string device_id, const PartConfig& part,
    std::shared_ptr<ext::simulator::ipc::SimulatorClient> simulator_world);
}  // namespace eos::runtime::hardware_runtime::detail

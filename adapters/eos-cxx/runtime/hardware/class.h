#pragma once

#include <string>
#include <toml++/toml.hpp>

#include "binding.h"

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

void ApplyBaseDefaults(PartConfig& config);
void ParseBasePart(const toml::table& table, PartConfig& config, const std::string& path,
                   const std::string& where);

void ApplyCameraDefaults(PartConfig& config);
void ParseCameraPart(const toml::table& table, PartConfig& config, const std::string& path,
                     const std::string& where);

void ApplyGimbalDefaults(PartConfig& config);
void ParseGimbalPart(const toml::table& table, PartConfig& config, const std::string& path,
                     const std::string& where);

void ParseGripperPart(const toml::table& table, PartConfig& config, const std::string& path,
                      const std::string& where);

void ParseMicPart(const toml::table& table, PartConfig& config, const std::string& path,
                  const std::string& where);

void ParseSpeakerPart(const toml::table& table, PartConfig& config, const std::string& path,
                      const std::string& where);

}  // namespace eos::runtime::hardware_runtime::detail

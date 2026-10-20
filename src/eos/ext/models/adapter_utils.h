#pragma once

#include <cstdint>
#include <exception>
#include <string>
#include <vector>

#include "src/eos/runtime/models/runtime.h"

namespace eos::ext::models::detail {
nlohmann::json ParseJson(std::string_view body);
std::string EncodeBase64(std::string_view input);
std::vector<std::uint8_t> DecodeBase64(std::string_view input);
runtime::models::CallResult Cancelled(const runtime::models::ModelConfig& model);
runtime::models::CallResult StreamException(const runtime::models::ModelConfig& model,
                                            const std::exception& error);
void NormalizeVisionPeople(runtime::models::Capability capability, nlohmann::json* response);
void Require(bool condition, const runtime::models::ModelConfig& model,
             const std::string& requirement);
}  // namespace eos::ext::models::detail

#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace eos::backends::ros2::socket_protocol {
using Json = nlohmann::json;
constexpr int kVersion = 1;
constexpr std::size_t kMaxMessage = 32 * 1024 * 1024;

// One versioned JSON request and response per Unix stream connection. Each
// message is newline terminated; binary image data uses bounded byte arrays.
Json Exchange(const std::string& socket_path, const Json& request, int timeout_seconds = 2);
Json Read(int fd);
void Write(int fd, const Json& message);
}  // namespace eos::backends::ros2::socket_protocol

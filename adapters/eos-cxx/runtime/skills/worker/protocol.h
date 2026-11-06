#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <sys/types.h>

namespace eos::runtime::skills::worker {

// One packet is one request or response. The transport does not interpret the
// payload; higher layers must validate its schema and execution capability.
constexpr std::uint16_t kProtocolVersion = 1;  // Wire encoding for worker IPC 1.0.0.
constexpr const char* kProtocolSemanticVersion = "1.0.0";
constexpr std::size_t kMaxPayloadBytes = 64 * 1024;

enum class MessageType : std::uint16_t {
  kHello = 1,
  kHelloReply = 2,
  kCatalog = 3,
  kExecute = 4,
  kResult = 5,
  kCapabilityCall = 6,
  kCapabilityReply = 7,
  kCancel = 8,
  kError = 9,
  kStop = 10,
};

struct Message {
  MessageType type{MessageType::kError};
  std::uint64_t request_id{0};
  std::uint64_t generation{0};
  std::string payload;
  // Large immutable payloads may be passed as a sealed memfd. Ownership of
  // this descriptor remains with the caller; Receive owns its returned fd.
  int payload_fd{-1};
};

struct PeerCredentials {
  pid_t pid;
  uid_t uid;
  gid_t gid;
};

// Throws std::runtime_error on malformed, truncated, or failed I/O. Receive
// returns nullopt on clean peer shutdown. A received payload_fd must be closed.
void Send(int socket_fd, const Message& message);
std::optional<Message> Receive(int socket_fd);
PeerCredentials Credentials(int socket_fd);

}  // namespace eos::runtime::skills::worker

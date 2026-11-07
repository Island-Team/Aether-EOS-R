#include "adapters/eos-cxx/runtime/skills/worker/protocol.h"

#include <cerrno>
#include <fcntl.h>
#include <cstring>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace eos::runtime::skills::worker {
namespace {
constexpr std::uint32_t kMagic = 0x534f4549;  // IEOS, little endian.
constexpr std::size_t kHeaderBytes = 28;
constexpr std::uint16_t kHasFd = 1;

void Put16(unsigned char* p, std::uint16_t v) {
  p[0] = static_cast<unsigned char>(v);
  p[1] = static_cast<unsigned char>(v >> 8);
}
void Put32(unsigned char* p, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<unsigned char>(v >> (8 * i));
}
void Put64(unsigned char* p, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<unsigned char>(v >> (8 * i));
}
std::uint16_t Get16(const unsigned char* p) { return p[0] | (std::uint16_t(p[1]) << 8); }
std::uint32_t Get32(const unsigned char* p) {
  std::uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= std::uint32_t(p[i]) << (8 * i);
  return v;
}
std::uint64_t Get64(const unsigned char* p) {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= std::uint64_t(p[i]) << (8 * i);
  return v;
}
bool KnownType(std::uint16_t type) {
  return type >= static_cast<std::uint16_t>(MessageType::kHello) &&
         type <= static_cast<std::uint16_t>(MessageType::kStop);
}
void ValidateMemfd(int fd) {
  struct stat info{};
  if (fstat(fd, &info) != 0 || info.st_size < 0 || info.st_size > 64 * 1024 * 1024)
    throw std::runtime_error("worker payload memfd exceeds size limit");
  const int seals = fcntl(fd, F_GET_SEALS);
  if (seals < 0 || (seals & (F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK)) !=
                       (F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK))
    throw std::runtime_error("worker payload fd must be a sealed memfd");
}
}  // namespace

void Send(int socket_fd, const Message& message) {
  if (message.payload.size() > kMaxPayloadBytes)
    throw std::runtime_error("worker packet exceeds payload limit");
  if (!KnownType(static_cast<std::uint16_t>(message.type)))
    throw std::runtime_error("worker packet has unknown message type");
  if (message.payload_fd >= 0) ValidateMemfd(message.payload_fd);
  unsigned char header[kHeaderBytes]{};
  Put32(header, kMagic);
  Put16(header + 4, kProtocolVersion);
  Put16(header + 6, static_cast<std::uint16_t>(message.type));
  Put16(header + 8, message.payload_fd >= 0 ? kHasFd : 0);
  Put64(header + 12, message.request_id);
  Put64(header + 20, message.generation);
  // Payload length is implicit in the packet boundary. This prevents
  // length-field mismatch and preserves an exact upper bound.
  iovec iov[2]{{header, sizeof(header)},
               {const_cast<char*>(message.payload.data()), message.payload.size()}};
  msghdr msg{};
  msg.msg_iov = iov;
  msg.msg_iovlen = 2;
  char control[CMSG_SPACE(sizeof(int))]{};
  if (message.payload_fd >= 0) {
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(cmsg), &message.payload_fd, sizeof(int));
  }
  const ssize_t sent = sendmsg(socket_fd, &msg, MSG_NOSIGNAL);
  if (sent < 0) throw std::runtime_error(std::string("worker sendmsg: ") + std::strerror(errno));
  if (static_cast<std::size_t>(sent) != sizeof(header) + message.payload.size())
    throw std::runtime_error("worker packet was not sent atomically");
}

std::optional<Message> Receive(int socket_fd) {
  std::vector<unsigned char> bytes(kHeaderBytes + kMaxPayloadBytes);
  char control[CMSG_SPACE(sizeof(int) * 2)]{};
  iovec iov{bytes.data(), bytes.size()};
  msghdr msg{};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = control;
  msg.msg_controllen = sizeof(control);
  const ssize_t size = recvmsg(socket_fd, &msg, MSG_CMSG_CLOEXEC);
  if (size == 0) return std::nullopt;
  if (size < 0) throw std::runtime_error(std::string("worker recvmsg: ") + std::strerror(errno));
  int received_fd = -1;
  int fd_count = 0;
  for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
      const auto count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
      const auto* fds = reinterpret_cast<const int*>(CMSG_DATA(cmsg));
      for (std::size_t i = 0; i < count; ++i) {
        if (fd_count++ == 0) received_fd = fds[i];
        else close(fds[i]);
      }
    }
  }
  try {
    if ((msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || size < static_cast<ssize_t>(kHeaderBytes))
      throw std::runtime_error("worker packet is truncated");
    const auto* h = bytes.data();
    if (Get32(h) != kMagic || Get16(h + 4) != kProtocolVersion)
      throw std::runtime_error("worker packet has incompatible header");
    const auto type = Get16(h + 6);
    const auto flags = Get16(h + 8);
    if (!KnownType(type) || (flags & ~kHasFd) || h[10] != 0 || h[11] != 0 ||
        bool(flags & kHasFd) != (fd_count == 1) || fd_count > 1)
      throw std::runtime_error("worker packet has invalid type, flags, or descriptors");
    if (received_fd >= 0) ValidateMemfd(received_fd);
    Message result;
    result.type = static_cast<MessageType>(type);
    result.request_id = Get64(h + 12);
    result.generation = Get64(h + 20);
    result.payload.assign(reinterpret_cast<const char*>(h + kHeaderBytes),
                          static_cast<std::size_t>(size) - kHeaderBytes);
    result.payload_fd = received_fd;
    return result;
  } catch (...) {
    if (received_fd >= 0) close(received_fd);
    throw;
  }
}

PeerCredentials Credentials(int socket_fd) {
  ucred peer{};
  socklen_t size = sizeof(peer);
  if (getsockopt(socket_fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 || size != sizeof(peer))
    throw std::runtime_error("cannot authenticate worker peer");
  return {peer.pid, peer.uid, peer.gid};
}
}  // namespace eos::runtime::skills::worker

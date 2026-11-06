#include "socket_protocol.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace eos::backends::ros2::socket_protocol {
Json Read(int fd) {
  std::string data;
  data.reserve(4096);
  char block[4096];
  while (true) {
    const ssize_t n = recv(fd, block, sizeof(block), 0);
    if (n <= 0) throw std::runtime_error(n == 0 ? "bridge closed connection" : std::strerror(errno));
    const char* end = static_cast<const char*>(memchr(block, '\n', static_cast<std::size_t>(n)));
    data.append(block, end ? static_cast<std::size_t>(end - block) : static_cast<std::size_t>(n));
    if (data.size() > kMaxMessage) throw std::runtime_error("bridge message exceeds limit");
    if (end) return Json::parse(data);
  }
}
void Write(int fd, const Json& message) {
  const std::string data = message.dump() + "\n";
  if (data.size() > kMaxMessage) throw std::runtime_error("bridge message exceeds limit");
  std::size_t offset = 0;
  while (offset < data.size()) {
    const ssize_t n = send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) throw std::runtime_error(std::strerror(errno));
    offset += static_cast<std::size_t>(n);
  }
}
Json Exchange(const std::string& path, const Json& request, int timeout_seconds) {
  if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path))
    throw std::runtime_error("invalid bridge socket path");
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) throw std::runtime_error(std::strerror(errno));
  struct Close { int fd; ~Close() { close(fd); } } close_fd{fd};
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  timeval timeout{timeout_seconds, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
    throw std::runtime_error(std::strerror(errno));
  Write(fd, request);
  Json reply = Read(fd);
  if (reply.value("v", 0) != kVersion) throw std::runtime_error("bridge protocol version mismatch");
  if (!reply.value("ok", false)) throw std::runtime_error(reply.value("error", "bridge request failed"));
  return reply;
}
}  // namespace eos::backends::ros2::socket_protocol

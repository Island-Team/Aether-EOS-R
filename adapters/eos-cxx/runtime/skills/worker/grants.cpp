#include "adapters/eos-cxx/runtime/skills/worker/grants.h"

#include <sys/random.h>

#include <array>
#include <cerrno>
#include <stdexcept>

namespace eos::runtime::skills::worker {
namespace {
std::string RandomToken() {
  std::array<unsigned char, 32> bytes{};
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t count = getrandom(bytes.data() + offset, bytes.size() - offset, 0);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("cannot create worker capability token");
    offset += static_cast<std::size_t>(count);
  }
  constexpr char hex[] = "0123456789abcdef";
  std::string token;
  token.reserve(bytes.size() * 2);
  for (unsigned char byte : bytes) {
    token += hex[byte >> 4];
    token += hex[byte & 15];
  }
  return token;
}
}  // namespace

std::string GrantTable::Issue(Grant grant) {
  if (grant.execution_id.empty() || grant.primitive.empty() || grant.version.empty() ||
      grant.device_id.empty() || grant.expires_at <= std::chrono::steady_clock::now())
    throw std::invalid_argument("incomplete or expired worker capability grant");
  const std::string token = RandomToken();
  std::lock_guard<std::mutex> lock(mutex_);
  grants_.emplace(token, std::move(grant));
  return token;
}

bool GrantTable::Authorize(const CapabilityCall& call) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = grants_.find(call.token);
  if (it == grants_.end()) return false;
  const Grant& grant = it->second;
  return grant.expires_at > std::chrono::steady_clock::now() &&
         grant.execution_id == call.execution_id && grant.generation == call.generation &&
         grant.primitive == call.primitive && grant.version == call.version &&
         grant.device_id == call.device_id &&
         (grant.capability.empty() || grant.capability == call.capability);
}

void GrantTable::RevokeExecution(const std::string& execution_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = grants_.begin(); it != grants_.end();) {
    if (it->second.execution_id == execution_id) it = grants_.erase(it);
    else ++it;
  }
}

void GrantTable::RevokeGeneration(std::uint64_t generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = grants_.begin(); it != grants_.end();) {
    if (it->second.generation == generation) it = grants_.erase(it);
    else ++it;
  }
}
}  // namespace eos::runtime::skills::worker

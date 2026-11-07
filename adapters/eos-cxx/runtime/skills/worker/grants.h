#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <string>

namespace eos::runtime::skills::worker {

// Created by the binding manager after it resolves an execution's arguments.
// A grant never names an arbitrary endpoint supplied by the worker.
struct Grant {
  std::string execution_id;
  std::uint64_t generation{0};
  std::string primitive;
  std::string version;
  std::string device_id;
  std::chrono::steady_clock::time_point expires_at;
  // Stable part selector supplied by an execution argument, never a generated ID.
  std::string selector;
  // Canonical catalog name. Empty only for legacy standalone GrantTable users.
  std::string capability;
};

struct CapabilityCall {
  std::string token;
  std::string execution_id;
  std::uint64_t generation{0};
  std::string primitive;
  std::string version;
  std::string device_id;
  std::string capability;
};

// The broker must check a call immediately before forwarding it to Dispatcher.
// A cancelled execution or retired generation revokes all of its grants.
class GrantTable {
 public:
  std::string Issue(Grant grant);
  bool Authorize(const CapabilityCall& call) const;
  void RevokeExecution(const std::string& execution_id);
  void RevokeGeneration(std::uint64_t generation);

 private:
  mutable std::mutex mutex_;
  std::map<std::string, Grant> grants_;
};

}  // namespace eos::runtime::skills::worker

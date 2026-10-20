#include "src/eos/runtime/perception/producer_coordinator.h"

#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace eos::runtime::perception {
namespace detail {

struct ProducerNode {
  std::string name;
  std::vector<std::size_t> dependencies;
  ProducerSource::Factory factory;
  ProducerSource::Stop stop;
  ProducerStatus status;
};

struct ProducerCoordinatorState {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<ProducerNode> nodes;
  std::unordered_map<std::string, std::size_t> by_name;
  std::vector<std::vector<std::size_t>> closures;
};

}  // namespace detail
namespace {

using State = detail::ProducerCoordinatorState;

std::size_t Lookup(const State& state, std::string_view name) {
  const auto found = state.by_name.find(std::string(name));
  if (found == state.by_name.end()) {
    throw std::invalid_argument("unknown producer source: " + std::string(name));
  }
  return found->second;
}

void BuildClosure(const State& state, std::size_t node, std::vector<unsigned char>& visiting,
                  std::vector<unsigned char>& included, std::vector<std::size_t>& result) {
  if (visiting[node] != 0) {
    throw std::invalid_argument("producer dependency graph contains a cycle");
  }
  if (included[node] != 0) {
    return;
  }
  visiting[node] = 1;
  for (const auto dependency : state.nodes[node].dependencies) {
    BuildClosure(state, dependency, visiting, included, result);
  }
  visiting[node] = 0;
  included[node] = 1;
  result.push_back(node);
}

bool IsStable(const State& state, const std::vector<std::size_t>& closure) {
  for (const auto node : closure) {
    const auto lifecycle = state.nodes[node].status.lifecycle;
    if (lifecycle == ProducerLifecycle::kStarting ||
        lifecycle == ProducerLifecycle::kStopping) {
      return false;
    }
  }
  return true;
}

void Release(const std::shared_ptr<State>& state, std::size_t source) noexcept {
  std::vector<std::pair<std::size_t, ProducerSource::Stop>> stopping;
  stopping.reserve(state->closures[source].size());
  {
    std::unique_lock<std::mutex> lock(state->mutex);
    const auto& closure = state->closures[source];
    state->changed.wait(lock, [&] { return IsStable(*state, closure); });
    for (const auto node : closure) {
      --state->nodes[node].status.consumer_count;
    }
    for (auto it = closure.rbegin(); it != closure.rend(); ++it) {
      auto& producer = state->nodes[*it];
      if (producer.status.consumer_count == 0 &&
          producer.status.lifecycle == ProducerLifecycle::kRunning) {
        producer.status.lifecycle = ProducerLifecycle::kStopping;
        stopping.emplace_back(*it, std::move(producer.stop));
      }
    }
  }

  for (auto& [node, stop] : stopping) {
    try {
      if (stop) {
        stop();
      }
    } catch (...) {
      // Lease destruction cannot report stop failures; the producer is still
      // considered stopped so future demand can start a fresh instance.
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    auto& producer = state->nodes[node];
    producer.status.lifecycle = ProducerLifecycle::kStopped;
    ++producer.status.stop_count;
  }
  state->changed.notify_all();
}

}  // namespace

ProducerLease::ProducerLease(std::shared_ptr<State> state, std::size_t source,
                             bool activated) noexcept
    : state_(std::move(state)), source_(source), activated_(activated) {}

ProducerLease::~ProducerLease() { Reset(); }

ProducerLease::ProducerLease(ProducerLease&& other) noexcept
    : state_(std::move(other.state_)), source_(other.source_), activated_(other.activated_) {}

ProducerLease& ProducerLease::operator=(ProducerLease&& other) noexcept {
  if (this != &other) {
    Reset();
    state_ = std::move(other.state_);
    source_ = other.source_;
    activated_ = other.activated_;
  }
  return *this;
}

void ProducerLease::Reset() noexcept {
  auto state = std::move(state_);
  if (state) {
    Release(state, source_);
  }
}

ProducerCoordinator::ProducerCoordinator(std::vector<ProducerSource> sources)
    : state_(std::make_shared<State>()) {
  state_->nodes.reserve(sources.size());
  state_->closures.resize(sources.size());
  for (std::size_t i = 0; i < sources.size(); ++i) {
    if (sources[i].name.empty() || !sources[i].factory) {
      throw std::invalid_argument("producer sources require a name and factory");
    }
    if (!state_->by_name.emplace(sources[i].name, i).second) {
      throw std::invalid_argument("duplicate producer source: " + sources[i].name);
    }
    state_->nodes.push_back({std::move(sources[i].name), {}, std::move(sources[i].factory), {}, {}});
  }
  for (std::size_t i = 0; i < sources.size(); ++i) {
    for (const auto& dependency : sources[i].dependencies) {
      state_->nodes[i].dependencies.push_back(Lookup(*state_, dependency));
    }
  }
  for (std::size_t i = 0; i < sources.size(); ++i) {
    std::vector<unsigned char> visiting(sources.size());
    std::vector<unsigned char> included(sources.size());
    BuildClosure(*state_, i, visiting, included, state_->closures[i]);
  }
}

ProducerLease ProducerCoordinator::Acquire(std::string_view source) const {
  const auto state = state_;
  if (!state) {
    throw std::logic_error("producer coordinator has been moved from");
  }

  std::size_t source_index;
  std::vector<std::size_t> starting;
  {
    std::unique_lock<std::mutex> lock(state->mutex);
    source_index = Lookup(*state, source);
    const auto& closure = state->closures[source_index];
    state->changed.wait(lock, [&] { return IsStable(*state, closure); });
    starting.reserve(closure.size());
    for (const auto node : closure) {
      auto& producer = state->nodes[node];
      if (producer.status.consumer_count++ == 0) {
        producer.status.lifecycle = ProducerLifecycle::kStarting;
        starting.push_back(node);
      }
    }
  }

  try {
    for (const auto node : starting) {
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        ++state->nodes[node].status.start_attempt_count;
      }
      auto stop = state->nodes[node].factory();
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        auto& producer = state->nodes[node];
        producer.stop = std::move(stop);
        producer.status.lifecycle = ProducerLifecycle::kRunning;
        ++producer.status.start_count;
      }
    }
  } catch (...) {
    const auto failure = std::current_exception();
    std::vector<std::pair<std::size_t, ProducerSource::Stop>> stopping;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      const auto& closure = state->closures[source_index];
      for (const auto node : closure) {
        --state->nodes[node].status.consumer_count;
      }
      for (const auto node : starting) {
        auto& producer = state->nodes[node];
        if (producer.status.consumer_count == 0 &&
            producer.status.lifecycle == ProducerLifecycle::kStarting) {
          producer.status.lifecycle = ProducerLifecycle::kStopped;
        }
      }
      for (auto it = closure.rbegin(); it != closure.rend(); ++it) {
        auto& producer = state->nodes[*it];
        if (producer.status.consumer_count == 0 &&
            producer.status.lifecycle == ProducerLifecycle::kRunning) {
          producer.status.lifecycle = ProducerLifecycle::kStopping;
          stopping.emplace_back(*it, std::move(producer.stop));
        }
      }
    }
    for (auto& [node, stop] : stopping) {
      try {
        if (stop) {
          stop();
        }
      } catch (...) {
      }
      std::lock_guard<std::mutex> lock(state->mutex);
      auto& producer = state->nodes[node];
      producer.status.lifecycle = ProducerLifecycle::kStopped;
      ++producer.status.stop_count;
    }
    state->changed.notify_all();
    std::rethrow_exception(failure);
  }

  state->changed.notify_all();
  return ProducerLease(state, source_index, !starting.empty());
}

ProducerStatus ProducerCoordinator::Status(std::string_view source) const {
  const auto state = state_;
  if (!state) {
    throw std::logic_error("producer coordinator has been moved from");
  }
  std::lock_guard<std::mutex> lock(state->mutex);
  return state->nodes[Lookup(*state, source)].status;
}

}  // namespace eos::runtime::perception

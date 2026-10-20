#include "src/eos/runtime/perception/producer_coordinator.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace eos::runtime::perception {
namespace {

using namespace std::chrono_literals;

class Gate {
 public:
  void ArriveAndWait() {
    std::unique_lock<std::mutex> lock(mutex_);
    arrived_ = true;
    changed_.notify_all();
    changed_.wait(lock, [&] { return open_; });
  }

  void WaitForArrival() {
    std::unique_lock<std::mutex> lock(mutex_);
    ASSERT_TRUE(changed_.wait_for(lock, 2s, [&] { return arrived_; }));
  }

  void Open() {
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = true;
    changed_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool arrived_{false};
  bool open_{false};
};

TEST(ProducerCoordinatorTest, ValidatesNamedDependencyGraph) {
  EXPECT_THROW(ProducerCoordinator({{"a", {"missing"}, [] { return [] {}; }}}),
               std::invalid_argument);
  EXPECT_THROW(ProducerCoordinator({{"a", {"b"}, [] { return [] {}; }},
                                    {"b", {"a"}, [] { return [] {}; }}}),
               std::invalid_argument);
  EXPECT_THROW(ProducerCoordinator({{"a", {}, [] { return [] {}; }},
                                    {"a", {}, [] { return [] {}; }}}),
               std::invalid_argument);
}

TEST(ProducerCoordinatorTest, LeaseIsMoveOnlyAndStartsOnFirstTransitiveConsumer) {
  static_assert(!std::is_copy_constructible_v<ProducerLease>);
  static_assert(std::is_move_constructible_v<ProducerLease>);
  std::vector<std::string> events;
  ProducerCoordinator coordinator({
      {"camera", {}, [&] {
         events.push_back("start camera");
         return [&] { events.push_back("stop camera"); };
       }},
      {"detector", {"camera"}, [&] {
         events.push_back("start detector");
         return [&] { events.push_back("stop detector"); };
       }},
  });

  auto first = coordinator.Acquire("detector");
  auto second = coordinator.Acquire("detector");
  EXPECT_EQ(events, (std::vector<std::string>{"start camera", "start detector"}));
  EXPECT_EQ(coordinator.Status("camera").consumer_count, 2u);
  EXPECT_EQ(coordinator.Status("camera").start_count, 1u);

  first.Reset();
  EXPECT_EQ(coordinator.Status("detector").lifecycle, ProducerLifecycle::kRunning);
  second.Reset();
  EXPECT_EQ(events, (std::vector<std::string>{"start camera", "start detector",
                                              "stop detector", "stop camera"}));
  EXPECT_EQ(coordinator.Status("camera").stop_count, 1u);
}

TEST(ProducerCoordinatorTest, SharesDependenciesUntilTheirLastConsumerLeaves) {
  std::atomic<int> camera_stops{0};
  ProducerCoordinator observed({
      {"camera", {}, [&] { return [&] { ++camera_stops; }; }},
      {"faces", {"camera"}, [] { return [] {}; }},
      {"poses", {"camera"}, [] { return [] {}; }},
  });

  auto faces = observed.Acquire("faces");
  auto poses = observed.Acquire("poses");
  faces.Reset();
  EXPECT_EQ(camera_stops, 0);
  EXPECT_EQ(observed.Status("camera").consumer_count, 1u);
  poses.Reset();
  EXPECT_EQ(camera_stops, 1);
  EXPECT_EQ(observed.Status("camera").lifecycle, ProducerLifecycle::kStopped);
}

TEST(ProducerCoordinatorTest, ConcurrentConsumersRunOneFactoryAndOneStop) {
  Gate start_gate;
  std::atomic<int> starts{0};
  std::atomic<int> stops{0};
  ProducerCoordinator coordinator({{"source", {}, [&] {
                                      ++starts;
                                      start_gate.ArriveAndWait();
                                      return [&] { ++stops; };
                                    }}});

  auto first = std::async(std::launch::async, [&] { return coordinator.Acquire("source"); });
  start_gate.WaitForArrival();
  auto second = std::async(std::launch::async, [&] { return coordinator.Acquire("source"); });
  EXPECT_EQ(second.wait_for(50ms), std::future_status::timeout);
  EXPECT_EQ(coordinator.Status("source").lifecycle, ProducerLifecycle::kStarting);
  start_gate.Open();
  auto first_lease = first.get();
  auto second_lease = second.get();
  EXPECT_EQ(starts, 1);
  first_lease.Reset();
  second_lease.Reset();
  EXPECT_EQ(stops, 1);
}

TEST(ProducerCoordinatorTest, AcquisitionDuringStoppingWaitsThenRestarts) {
  Gate stop_gate;
  std::atomic<int> starts{0};
  ProducerCoordinator coordinator({{"source", {}, [&] {
                                      ++starts;
                                      return [&] { stop_gate.ArriveAndWait(); };
                                    }}});
  auto lease = coordinator.Acquire("source");
  auto release = std::async(std::launch::async, [&] { lease.Reset(); });
  stop_gate.WaitForArrival();

  EXPECT_EQ(coordinator.Status("source").lifecycle, ProducerLifecycle::kStopping);
  auto reacquire = std::async(std::launch::async, [&] { return coordinator.Acquire("source"); });
  EXPECT_EQ(reacquire.wait_for(50ms), std::future_status::timeout);
  stop_gate.Open();
  release.get();
  auto restarted = reacquire.get();
  EXPECT_EQ(starts, 2);
  EXPECT_EQ(coordinator.Status("source").lifecycle, ProducerLifecycle::kRunning);
}

TEST(ProducerCoordinatorTest, StatusDoesNotWaitForAStoppingFactory) {
  Gate stop_gate;
  ProducerCoordinator coordinator(
      {{"source", {}, [&] { return [&] { stop_gate.ArriveAndWait(); }; }}});
  auto lease = coordinator.Acquire("source");
  auto release = std::async(std::launch::async, [&] { lease.Reset(); });
  stop_gate.WaitForArrival();

  auto status = std::async(std::launch::async, [&] { return coordinator.Status("source"); });
  const auto wait_result = status.wait_for(500ms);
  stop_gate.Open();
  release.get();
  ASSERT_EQ(wait_result, std::future_status::ready);
  EXPECT_EQ(status.get().lifecycle, ProducerLifecycle::kStopping);
}

TEST(ProducerCoordinatorTest, IndependentAcquisitionDoesNotWaitForStoppingFactory) {
  Gate stop_gate;
  ProducerCoordinator coordinator({
      {"stopping", {}, [&] { return [&] { stop_gate.ArriveAndWait(); }; }},
      {"independent", {}, [] { return [] {}; }},
  });
  auto lease = coordinator.Acquire("stopping");
  auto release = std::async(std::launch::async, [&] { lease.Reset(); });
  stop_gate.WaitForArrival();

  auto acquire = std::async(std::launch::async, [&] {
    return coordinator.Acquire("independent");
  });
  const auto wait_result = acquire.wait_for(500ms);
  stop_gate.Open();
  release.get();
  ASSERT_EQ(wait_result, std::future_status::ready);
  auto independent = acquire.get();
  EXPECT_EQ(coordinator.Status("independent").lifecycle, ProducerLifecycle::kRunning);
}

TEST(ProducerCoordinatorTest, FailedStartupRollsBackAndCanBeRetried) {
  std::atomic<int> dependency_starts{0};
  std::atomic<int> dependency_stops{0};
  std::atomic<int> source_attempts{0};
  ProducerCoordinator coordinator({
      {"dependency", {}, [&] {
         ++dependency_starts;
         return [&] { ++dependency_stops; };
       }},
      {"source", {"dependency"}, [&] {
         if (++source_attempts == 1) {
           throw std::runtime_error("start failed");
         }
         return [] {};
       }},
  });

  EXPECT_THROW(coordinator.Acquire("source"), std::runtime_error);
  EXPECT_EQ(coordinator.Status("dependency").lifecycle, ProducerLifecycle::kStopped);
  EXPECT_EQ(coordinator.Status("dependency").consumer_count, 0u);
  EXPECT_EQ(coordinator.Status("dependency").start_count, 1u);
  EXPECT_EQ(coordinator.Status("dependency").stop_count, 1u);
  EXPECT_EQ(coordinator.Status("source").start_attempt_count, 1u);
  EXPECT_EQ(coordinator.Status("source").start_count, 0u);

  auto lease = coordinator.Acquire("source");
  EXPECT_EQ(dependency_starts, 2);
  EXPECT_EQ(source_attempts, 2);
  lease.Reset();
  EXPECT_EQ(dependency_stops, 2);
}

TEST(ProducerCoordinatorTest, FailedAcquisitionKeepsDependencyUsedBySurvivingLeaseRunning) {
  Gate failure_gate;
  std::atomic<int> dependency_stops{0};
  ProducerCoordinator coordinator({
      {"dependency", {}, [&] { return [&] { ++dependency_stops; }; }},
      {"failing", {"dependency"}, [&]() -> ProducerSource::Stop {
         failure_gate.ArriveAndWait();
         throw std::runtime_error("start failed");
       }},
      {"survivor", {"dependency"}, [] { return [] {}; }},
  });

  auto failed = std::async(std::launch::async, [&] {
    EXPECT_THROW(coordinator.Acquire("failing"), std::runtime_error);
  });
  failure_gate.WaitForArrival();
  auto survivor = coordinator.Acquire("survivor");
  failure_gate.Open();
  failed.get();

  EXPECT_EQ(dependency_stops, 0);
  EXPECT_EQ(coordinator.Status("dependency").lifecycle, ProducerLifecycle::kRunning);
  EXPECT_EQ(coordinator.Status("dependency").consumer_count, 1u);
  survivor.Reset();
  EXPECT_EQ(dependency_stops, 1);
}

TEST(ProducerCoordinatorTest, LastConcurrentRollbackStopsZeroConsumerDependency) {
  Gate first_failure_gate;
  Gate second_failure_gate;
  std::atomic<int> dependency_stops{0};
  ProducerCoordinator coordinator({
      {"dependency", {}, [&] { return [&] { ++dependency_stops; }; }},
      {"first", {"dependency"}, [&]() -> ProducerSource::Stop {
         first_failure_gate.ArriveAndWait();
         throw std::runtime_error("first failed");
       }},
      {"second", {"dependency"}, [&]() -> ProducerSource::Stop {
         second_failure_gate.ArriveAndWait();
         throw std::runtime_error("second failed");
       }},
  });

  auto first = std::async(std::launch::async, [&] {
    EXPECT_THROW(coordinator.Acquire("first"), std::runtime_error);
  });
  first_failure_gate.WaitForArrival();
  auto second = std::async(std::launch::async, [&] {
    EXPECT_THROW(coordinator.Acquire("second"), std::runtime_error);
  });
  second_failure_gate.WaitForArrival();

  first_failure_gate.Open();
  first.get();
  EXPECT_EQ(dependency_stops, 0);
  EXPECT_EQ(coordinator.Status("dependency").consumer_count, 1u);
  second_failure_gate.Open();
  second.get();
  EXPECT_EQ(dependency_stops, 1);
  EXPECT_EQ(coordinator.Status("dependency").consumer_count, 0u);
  EXPECT_EQ(coordinator.Status("dependency").lifecycle, ProducerLifecycle::kStopped);
}

TEST(ProducerCoordinatorTest, ConcurrentReleaseStopsExactlyOnce) {
  std::atomic<int> stops{0};
  ProducerCoordinator coordinator(
      {{"source", {}, [&] { return [&] { ++stops; }; }}});
  auto first = coordinator.Acquire("source");
  auto second = coordinator.Acquire("source");

  auto one = std::async(std::launch::async, [&] { first.Reset(); });
  auto two = std::async(std::launch::async, [&] { second.Reset(); });
  one.get();
  two.get();
  EXPECT_EQ(stops, 1);
  EXPECT_EQ(coordinator.Status("source").consumer_count, 0u);
  EXPECT_EQ(coordinator.Status("source").stop_count, 1u);
}

}  // namespace
}  // namespace eos::runtime::perception

#include "share/skills/native/head_find_trash/head_find_trash.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "src/eos/lib/timer.h"
#include "src/eos/runtime/skills/builtin/registry.h"

namespace island::skills::native {
namespace {
using namespace eos::runtime::skills;
using namespace eos::runtime::skills::builtin;
using Json = nlohmann::json;

struct Call {
  std::string name;
  Json args;
};

Json Args() {
  return {{"robot_name", "demo-0"}, {"speaker_device_id", "speaker-0"}};
}

Json Snapshot(std::uint64_t sequence, const std::string& label, double confidence) {
  return {{"source", "cam-0"},
          {"source_sequence", sequence},
          {"detections", {{{"id", "obj:" + label},
                           {"kind", "object"},
                           {"label", label},
                           {"confidence", confidence},
                           {"bbox", Json::array({10, 10, 20, 20})}}}}};
}

Json Ack() {
  return {{"accepted", true}, {"reason", nullptr}, {"result", {{"status", "succeeded"}}}};
}

class HeadFindTrashTest : public testing::Test {
 protected:
  eos::lib::ThreadTimerService timer_;
};

TEST_F(HeadFindTrashTest, DeclaresSpeakerPlayInsteadOfSpeak) {
  Registry registry(timer_);
  RegisterHeadFindTrash(registry);
  const auto* metadata = registry.Find("head_find_trash", "1.0.0");
  ASSERT_NE(metadata, nullptr);
  ASSERT_EQ(metadata->primitives.size(), 1u);
  EXPECT_EQ(metadata->primitives.front().name, "speaker.play");
  EXPECT_EQ(metadata->primitives.front().version, "1.0.0");
}

TEST_F(HeadFindTrashTest, ValidatesSearchTimeout) {
  Registry registry(timer_);
  RegisterHeadFindTrash(registry);

  for (const int timeout : {0, 1000, 300000}) {
    Json arguments = Args();
    arguments["search_timeout_ms"] = timeout;
    EXPECT_NO_THROW(registry.ValidateArguments("head_find_trash", "1.0.0", arguments));
  }
  for (const int timeout : {-1, 1, 999, 300001}) {
    Json arguments = Args();
    arguments["search_timeout_ms"] = timeout;
    EXPECT_THROW(registry.ValidateArguments("head_find_trash", "1.0.0", arguments),
                 std::invalid_argument);
  }
  for (const Json timeout : {Json(1000.5), Json("1000")}) {
    Json arguments = Args();
    arguments["search_timeout_ms"] = timeout;
    EXPECT_THROW(registry.ValidateArguments("head_find_trash", "1.0.0", arguments),
                 std::invalid_argument);
  }
}

TEST_F(HeadFindTrashTest, RejectsWhenSearchTimesOut) {
  std::atomic<std::uint64_t> reads{0};
  RuntimeCapabilities capabilities;
  capabilities.read_resource = [&](const std::string&) {
    return Snapshot(++reads, "cup", 0.99);
  };
  Registry registry(timer_, std::move(capabilities));
  RegisterHeadFindTrash(registry);
  Json arguments = Args();
  arguments["search_timeout_ms"] = 1000;

  const auto result = registry.Execute("head_find_trash", "1.0.0", arguments, "request-timeout");

  EXPECT_EQ(result.status, RunResult::Status::kCompleted);
  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.reason_code, "trash.search_timeout");
  EXPECT_EQ(result.result, Json({{"found", false}, {"object_count", 0}}));
}

TEST_F(HeadFindTrashTest, PlaysLabelSpecificAssetOnStableDetection) {
  std::atomic<std::uint64_t> reads{0};
  std::atomic<int> announcements{0};
  std::mutex mutex;
  std::vector<Call> calls;
  RuntimeCapabilities capabilities;
  capabilities.execute_primitive = [&](const PrimitiveReference& primitive, const std::string&,
                                       Json arguments, const std::string&, Cancelled) {
    EXPECT_EQ(primitive.name, "speaker.play");
    EXPECT_EQ(primitive.version, "1.0.0");
    {
      std::lock_guard lock(mutex);
      calls.push_back({primitive.name, std::move(arguments)});
    }
    ++announcements;
    return Ack();
  };
  capabilities.read_resource = [&](const std::string& uri) {
    EXPECT_EQ(uri, "robots/demo-0/detection/head/state");
    return Snapshot(++reads, "empty_box", 0.9);
  };
  Registry registry(timer_, std::move(capabilities));
  RegisterHeadFindTrash(registry);
  ExecutionHooks hooks;
  hooks.cancelled = [&] { return announcements.load() > 0; };
  const auto result = registry.ExecuteWithHooks("head_find_trash", "1.0.0", Args(), "request-1",
                                                std::move(hooks));
  EXPECT_EQ(result.status, RunResult::Status::kCancelled) << result.reason_code;
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].name, "speaker.play");
  EXPECT_EQ(calls[0].args, Json({{"uri", "asset://head_find_trash/found_empty_box.wav"}}));
}

TEST_F(HeadFindTrashTest, CompletesAfterFirstAnnouncementWhenRequested) {
  std::atomic<std::uint64_t> reads{0};
  RuntimeCapabilities capabilities;
  capabilities.execute_primitive = [](const PrimitiveReference&, const std::string&, Json,
                                      const std::string&, Cancelled) { return Ack(); };
  capabilities.read_resource = [&](const std::string&) {
    return Snapshot(++reads, "bottle", 0.9);
  };
  Registry registry(timer_, std::move(capabilities));
  RegisterHeadFindTrash(registry);
  Json arguments = Args();
  arguments["stop_after_first_detection"] = true;

  const auto result = registry.Execute("head_find_trash", "1.0.0", arguments, "request-once");

  EXPECT_EQ(result.status, RunResult::Status::kCompleted) << result.reason_code;
  EXPECT_TRUE(result.result.at("found").get<bool>());
  EXPECT_EQ(result.result.at("label"), "bottle");
  EXPECT_EQ(result.result.at("announcement"), "发现空瓶了");
  EXPECT_GE(reads.load(), 3u);
}

TEST_F(HeadFindTrashTest, ReturnSchemaAcceptsEmptyBox) {
  Registry registry(timer_);
  RegisterHeadFindTrash(registry);
  const auto* metadata = registry.Find("head_find_trash", "1.0.0");
  ASSERT_NE(metadata, nullptr);
  const auto& labels = metadata->returns_schema.at("properties").at("label").at("enum");
  EXPECT_NE(std::find(labels.begin(), labels.end(), "empty_box"), labels.end());
}

TEST_F(HeadFindTrashTest, IgnoresUnsupportedLabelsAndLowConfidence) {
  std::atomic<std::uint64_t> reads{0};
  std::atomic<int> announcements{0};
  std::mutex mutex;
  std::vector<Call> calls;
  RuntimeCapabilities capabilities;
  capabilities.execute_primitive = [&](const PrimitiveReference& primitive, const std::string&,
                                       Json arguments, const std::string&, Cancelled) {
    std::lock_guard lock(mutex);
    calls.push_back({primitive.name, std::move(arguments)});
    ++announcements;
    return Ack();
  };
  capabilities.read_resource = [&](const std::string&) {
    const std::uint64_t sequence = ++reads;
    if (sequence % 2 == 0) return Snapshot(sequence, "cup", 0.99);
    return Snapshot(sequence, "bottle", 0.39);
  };
  Registry registry(timer_, std::move(capabilities));
  RegisterHeadFindTrash(registry);
  ExecutionHooks hooks;
  hooks.cancelled = [&] { return reads.load() >= 30; };
  const auto result = registry.ExecuteWithHooks("head_find_trash", "1.0.0", Args(), "request-2",
                                                std::move(hooks));
  EXPECT_EQ(result.status, RunResult::Status::kCancelled) << result.reason_code;
  EXPECT_TRUE(calls.empty());
}

}  // namespace
}  // namespace island::skills::native

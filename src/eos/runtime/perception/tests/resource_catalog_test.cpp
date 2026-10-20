#include "src/eos/runtime/perception/resource_catalog.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>

namespace eos::runtime::perception {
namespace {

using namespace std::chrono_literals;

void AcceptTestProduct(const nlohmann::json&) {}

TEST(ResourceCatalogTest, ListsDeclaredResourceBeforeFirstOutputAndColdReadDrivesProducer) {
  ProductHistory history(
      {"robots/demo/detection/state", "schema", "boot", "epoch", AcceptTestProduct}, 4);
  auto writer = history.TakeWriter();
  std::atomic<int> starts{0};
  std::atomic<int> stops{0};
  ProducerCoordinator producers({{"detection", {}, [&] {
                                    ++starts;
                                    writer.Append(std::chrono::system_clock::now(),
                                                  {{"detections", nlohmann::json::array()}});
                                    return [&] { ++stops; };
                                  }}});
  ResourceCatalog catalog;
  catalog.Register(history.Reader(), [&] { return producers.Acquire("detection"); }, 100ms);

  const auto listed = catalog.ListResources();
  ASSERT_EQ(listed.size(), 1u);
  EXPECT_EQ(listed.at(0).at("uri"), "robots/demo/detection/state");
  EXPECT_EQ(starts, 0);

  EXPECT_TRUE(catalog.ReadResource("robots/demo/detection/state").at("detections").empty());
  EXPECT_EQ(starts, 1);
  EXPECT_EQ(stops, 1);
  EXPECT_EQ(producers.Status("detection").consumer_count, 0u);
}

TEST(ResourceCatalogTest, DemandLeasesShareProducerUntilLastConsumerLeaves) {
  ProductHistory history({"resource", "schema", "boot", "epoch", AcceptTestProduct}, 2);
  history.TakeWriter();
  std::atomic<int> starts{0};
  std::atomic<int> stops{0};
  ProducerCoordinator producers({{"source", {}, [&] {
                                    ++starts;
                                    return [&] { ++stops; };
                                  }}});
  ResourceCatalog catalog;
  catalog.Register(history.Reader(), [&] { return producers.Acquire("source"); }, 100ms);

  auto first = catalog.AcquireDemand("resource");
  auto second = catalog.AcquireDemand("resource");
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_EQ(starts, 1);
  first.reset();
  EXPECT_EQ(stops, 0);
  second.reset();
  EXPECT_EQ(stops, 1);
}

TEST(ResourceCatalogTest, RepeatedTransientReadsWaitForEachActivation) {
  ProductHistory history({"resource", "schema", "boot", "epoch", AcceptTestProduct}, 4);
  auto writer = history.TakeWriter();
  std::atomic<int> starts{0};
  ProducerCoordinator producers({{"source", {}, [&] {
                                    const int value = ++starts;
                                    writer.Append(std::chrono::system_clock::now(),
                                                  {{"value", value}});
                                    return [] {};
                                  }}});
  ResourceCatalog catalog;
  catalog.Register(history.Reader(), [&] { return producers.Acquire("source"); }, 100ms);

  EXPECT_EQ(catalog.ReadResource("resource").at("value"), 1);
  EXPECT_EQ(catalog.ReadResource("resource").at("value"), 2);
  EXPECT_EQ(catalog.ReadResource("resource").at("value"), 3);
  EXPECT_EQ(starts, 3);
  EXPECT_EQ(producers.Status("source").consumer_count, 0u);
}

TEST(ResourceCatalogTest, ActiveProducerReturnsLatestWithoutWaiting) {
  ProductHistory history({"resource", "schema", "boot", "epoch", AcceptTestProduct}, 2);
  auto writer = history.TakeWriter();
  ProducerCoordinator producers({{"source", {}, [&] {
                                    writer.Append(std::chrono::system_clock::now(), {{"value", 7}});
                                    return [] {};
                                  }}});
  ResourceCatalog catalog;
  catalog.Register(history.Reader(), [&] { return producers.Acquire("source"); }, 1s);
  auto always_on = producers.Acquire("source");

  auto read = std::async(std::launch::async, [&] { return catalog.ReadResource("resource"); });
  ASSERT_EQ(read.wait_for(100ms), std::future_status::ready);
  EXPECT_EQ(read.get().at("value"), 7);
}

TEST(ResourceCatalogTest, ColdReadRejectsStaleProductWhenRestartPublishesNothing) {
  ProductHistory history({"resource", "schema", "boot", "epoch", AcceptTestProduct}, 2);
  auto writer = history.TakeWriter();
  writer.Append(std::chrono::system_clock::now(), {{"value", 1}});
  ProducerCoordinator producers({{"source", {}, [] { return [] {}; }}});
  ResourceCatalog catalog;
  catalog.Register(history.Reader(), [&] { return producers.Acquire("source"); }, 20ms);

  EXPECT_THROW(catalog.ReadResource("resource"), std::runtime_error);
  EXPECT_EQ(producers.Status("source").lifecycle, ProducerLifecycle::kStopped);
}

TEST(ResourceCatalogTest, VadIsDeclaredBeforeOutputAndPublishesEachActualResult) {
  const std::string uri = "robots/demo/world/vad/state";
  ProductHistory history(
      {uri, "egia.contracts.perception.world.vad.v1", "boot", "epoch",
       [](const nlohmann::json& value) {
         if (value.contains("pcm") || value.contains("wav") || value.contains("utterance"))
           throw std::invalid_argument("raw audio is not a VAD product");
       }},
      4);
  auto writer = history.TakeWriter();
  ResourceCatalog catalog;
  catalog.Register(history.Reader());

  ASSERT_EQ(catalog.ListResources().size(), 1u);
  EXPECT_EQ(catalog.ListResources().at(0).at("uri"), uri);
  EXPECT_THROW(catalog.ReadResource(uri), std::runtime_error);

  writer.Append(std::chrono::system_clock::time_point{} + 1s,
                {{"type", "vad.snapshot"}, {"source_sequence", 7}, {"speech", false}});
  writer.Append(std::chrono::system_clock::time_point{} + 2s,
                {{"type", "vad.snapshot"}, {"source_sequence", 8}, {"speech", true}});
  const auto retained = history.Reader().RangeBySequence(1, 2, 4);
  ASSERT_EQ(retained.products.size(), 2u);
  EXPECT_EQ(retained.products[0]->value.at("source_sequence"), 7u);
  EXPECT_EQ(catalog.ReadResource(uri).at("source_sequence"), 8u);
}

TEST(ResourceCatalogTest, DynamicProviderListsReadsAndSupportsTemplates) {
  ResourceCatalog catalog;
  catalog.RegisterProvider(
      {[] { return nlohmann::json::array({{{"uri", "robots/demo/navigation/waypoints/home"}}}); },
       [](const std::string& uri) -> std::optional<nlohmann::json> {
         if (uri != "robots/demo/navigation/waypoints/home") return std::nullopt;
         return nlohmann::json{{"reference", "absolute"}, {"x_m", 1.0}};
       },
       [](const std::string& uri_template) {
         return uri_template == "robots/{robot_name}/navigation/waypoints/{waypoint_id}";
       }});

  ASSERT_EQ(catalog.ListResources().size(), 1U);
  EXPECT_EQ(catalog.ReadResource("robots/demo/navigation/waypoints/home").at("x_m"), 1.0);
  EXPECT_TRUE(
      catalog.SupportsResourceTemplate("robots/{robot_name}/navigation/waypoints/{waypoint_id}"));
  EXPECT_THROW(catalog.ReadResource("missing"), std::runtime_error);
}

}  // namespace
}  // namespace eos::runtime::perception

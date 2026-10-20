#include "src/eos/ext/repl/catalog.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace eos::ext::repl {
namespace {

using json = nlohmann::json;

class CatalogTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ =
        std::filesystem::temp_directory_path() /
        ("eos-repl-catalog-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_ / "ext/eos/actions");
  }
  void TearDown() override { std::filesystem::remove_all(root_); }

  std::filesystem::path root_;
};

TEST_F(CatalogTest, LoadsPrimitiveArgumentsAndSuggestedValues) {
  std::ofstream(root_ / "ext/eos/actions/base.v1.json")
      << R"({"x-island-primitive-version":"1.0.0","properties":{"type":{"const":"base.posture"},"command_id":{"type":"string"},"posture":{"description":"Body pose","enum":["stand","lie"]},"enabled":{"type":"boolean"}}})";
  Catalog catalog;
  std::string warning;
  ASSERT_TRUE(catalog.LoadContracts(root_.string(), &warning)) << warning;
  const auto arguments = catalog.ArgumentsFor("base.posture", "1.0.0");
  ASSERT_EQ(arguments.size(), 2u);
  EXPECT_EQ(arguments.at("posture").description, "Body pose");
  EXPECT_EQ(arguments.at("posture").values, (std::vector<std::string>{"stand", "lie"}));
  EXPECT_EQ(arguments.at("enabled").values, (std::vector<std::string>{"true", "false"}));
}

TEST_F(CatalogTest, TracksSkillAndSignalSchemasAndValidatesArguments) {
  Catalog catalog;
  catalog.Observe("ListSkills",
                  json{{"skills",
                        {{{"reference", {{"name", "demo.move"}, {"version", "1.0.0"}}},
                          {"parameters",
                           {{"type", "object"},
                            {"required", {"distance"}},
                            {"properties",
                             {{"distance", {{"type", "number"}, {"minimum", 0}, {"maximum", 2}}},
                              {"safe", {{"type", "boolean"}, {"default", true}}}}}}},
                          {"acceptedSignals",
                           {{{"name", "adjust"},
                             {"payloadSchema",
                              {{"type", "object"},
                               {"required", {"speed"}},
                               {"properties", {{"speed", {{"type", "number"}}}}}}}}}}}}}});
  EXPECT_EQ(catalog.Skills(), (std::vector<std::string>{"demo.move"}));
  EXPECT_EQ(catalog.SkillVersionsFor("demo.move"), (std::vector<std::string>{"1.0.0"}));
  const auto arguments = catalog.SkillArgumentsFor("demo.move", "1.0.0");
  ASSERT_EQ(arguments.size(), 2u);
  EXPECT_TRUE(arguments.at("distance").required);
  EXPECT_EQ(arguments.at("distance").type, "number");
  EXPECT_TRUE(arguments.at("safe").has_default);
  std::string error;
  EXPECT_TRUE(catalog.Validate(arguments, {{"distance", 1.5}}, &error)) << error;
  EXPECT_FALSE(catalog.Validate(arguments, {{"distance", 3.0}}, &error));
  EXPECT_FALSE(catalog.Validate(arguments, {{"unknown", 1}}, &error));

  catalog.Observe("GetExecution", {{"executionId", "exec-1"},
                                   {"reference", {{"name", "demo.move"}, {"version", "1.0.0"}}}});
  const auto signal = catalog.SignalArgumentsFor("exec-1", "adjust");
  ASSERT_EQ(signal.size(), 1u);
  EXPECT_TRUE(signal.at("speed").required);
  EXPECT_EQ(catalog.SignalNames(), (std::vector<std::string>{"adjust", "cancel"}));
}

TEST_F(CatalogTest, ResolvesUnambiguousSkillLaunchAndUniqueDeploymentValues) {
  Catalog catalog;
  catalog.Observe(
      "Discover",
      json{{"robot", {{"name", "demo-0"}}},
           {"devices", {{{"id", "gimbal-0"}, {"class", "gimbal"},
                          {"primitives", json::array()}}}}});
  catalog.Observe(
      "ListSkills",
      json{{"skills",
            {{{"reference", {{"name", "observe_human"}, {"version", "1.0.0"}}},
              {"parameters",
               {{"type", "object"},
                {"required", {"robot_name", "gimbal_device_id"}},
                {"properties",
                 {{"robot_name", {{"type", "string"}}},
                  {"gimbal_device_id", {{"type", "string"}}}}}}}}}}});
  json arguments = json::object();
  std::string version;
  std::vector<std::string> unresolved;
  std::string error;
  ASSERT_TRUE(catalog.PrepareSkillLaunch("observe_human", "", &arguments, &version,
                                         &unresolved, &error))
      << error;
  EXPECT_EQ(version, "1.0.0");
  EXPECT_EQ(arguments, json({{"robot_name", "demo-0"}, {"gimbal_device_id", "gimbal-0"}}));
  EXPECT_TRUE(unresolved.empty());

  arguments = {{"robot_name", "override"}};
  ASSERT_TRUE(catalog.PrepareSkillLaunch("observe_human", "1.0.0", &arguments, &version,
                                         &unresolved, &error));
  EXPECT_EQ(arguments.at("robot_name"), "override");
}

TEST_F(CatalogTest, RequiresVersionAndPromptsForAmbiguousRequiredValues) {
  Catalog catalog;
  catalog.Observe(
      "Discover",
      json{{"robot", {{"name", "demo-0"}}},
           {"devices",
            {{{"id", "speaker-0"}, {"class", "speaker"}, {"primitives", json::array()}},
             {{"id", "speaker-1"}, {"class", "speaker"}, {"primitives", json::array()}}}}});
  const json parameters =
      {{"type", "object"}, {"required", {"speaker_device_id"}},
       {"properties", {{"speaker_device_id", {{"type", "string"}}}}}};
  catalog.Observe(
      "ListSkills",
      json{{"skills",
            {{{"reference", {{"name", "speak"}, {"version", "1.0.0"}}},
              {"parameters", parameters}},
             {{"reference", {{"name", "speak"}, {"version", "2.0.0"}}},
              {"parameters", parameters}}}}});
  json arguments = json::object();
  std::string version;
  std::vector<std::string> unresolved;
  std::string error;
  EXPECT_FALSE(catalog.PrepareSkillLaunch("speak", "", &arguments, &version, &unresolved,
                                          &error));
  ASSERT_TRUE(catalog.PrepareSkillLaunch("speak", "1.0.0", &arguments, &version, &unresolved,
                                         &error));
  EXPECT_EQ(unresolved, (std::vector<std::string>{"speaker_device_id"}));
}

TEST(CatalogWorkflowTest, ValidatesWorkflowShapeAndDependenciesLocally) {
  std::string error;
  const json valid = {
      {"definition",
       {{"workflowId", "workflow-1"},
        {"actions",
         {{{"actionId", "stop"},
           {"primitive", {{"name", "robot.stop"}, {"version", "1.0.0"}, {"deviceId", ""}}},
           {"parameters", json::object()}},
          {{"actionId", "observe"},
           {"skill", {{"name", "island.observe_human"}, {"version", "2.1.0"}}},
           {"parameters", json::object()},
           {"dependsOn", {"stop"}}}}}}}};
  EXPECT_TRUE(Catalog::ValidateWorkflow(valid, &error)) << error;
  json invalid = valid;
  invalid["definition"]["actions"] = json::array();
  EXPECT_FALSE(Catalog::ValidateWorkflow(invalid, &error));
  invalid = valid;
  invalid["definition"]["actions"][1]["dependsOn"] = {"missing"};
  EXPECT_FALSE(Catalog::ValidateWorkflow(invalid, &error));
  invalid = valid;
  invalid["definition"]["actions"][0]["parameters"]["commandId"] = "owned-by-eos";
  EXPECT_FALSE(Catalog::ValidateWorkflow(invalid, &error));
  invalid = valid;
  invalid["definition"]["actions"][0]["dependsOn"] = {"observe"};
  EXPECT_FALSE(Catalog::ValidateWorkflow(invalid, &error));
  EXPECT_NE(error.find("acyclic"), std::string::npos);
}

TEST(CatalogWorkflowTest, AcceptsControlNoopAndOmittedParameters) {
  const json workflow =
      {{"definition",
        {{"workflowId", "workflow-1"},
         {"actions",
          {{{"actionId", "observe"},
            {"skill", {{"name", "observe"}, {"version", "1.0.0"}}}},
           {{"actionId", "stop"},
            {"control", {{"targetActionId", "observe"}, {"operation", "TERMINATE"}}},
            {"dependsOn", {"observe"}}},
           {{"actionId", "done"}, {"noop", json::object()}, {"dependsOn", {"stop"}}}}}}}};
  std::string error;
  EXPECT_TRUE(Catalog::ValidateWorkflow(workflow, &error)) << error;
}

TEST_F(CatalogTest, ObservesAndClearsRuntimeMetadata) {
  Catalog catalog;
  catalog.Observe(
      "Discover",
      json{{"robot", {{"name", "demo"}}},
           {"devices",
            {{{"id", "base-0"},
              {"class", "base"},
              {"primitives",
               {{{"name", "base.velocity"},
                 {"version", "1.0.0"},
                 {"limits", {{"linear_x_m_s", {{"minimum", -1.0}, {"maximum", 1.0}}}}}}}}}}}});
  catalog.Observe("ListResources", json::array({{{"class", "base"}, {"uri", "robots/r/base"}}}));
  EXPECT_EQ(catalog.DevicesFor("base.velocity", "1.0.0"), (std::vector<std::string>{"base-0"}));
  EXPECT_EQ(catalog.PrimitiveVersionsFor("base.velocity"), (std::vector<std::string>{"1.0.0"}));
  EXPECT_EQ(catalog.Resources(), (std::vector<std::string>{"robots/r/base"}));
  EXPECT_EQ(catalog.Robots(), (std::vector<std::string>{"demo"}));
  EXPECT_EQ(catalog.DevicesForClass("base"), (std::vector<std::string>{"base-0"}));
  catalog.ClearRemote();
  EXPECT_TRUE(catalog.Resources().empty());
  EXPECT_TRUE(catalog.DevicesFor("base.velocity", "1.0.0").empty());
  EXPECT_TRUE(catalog.Robots().empty());
  EXPECT_TRUE(catalog.DevicesForClass("base").empty());
}

TEST_F(CatalogTest, MissingOrMalformedContractTreeIsNonThrowing) {
  Catalog catalog;
  std::string warning;
  EXPECT_FALSE(catalog.LoadContracts((root_ / "missing").string(), &warning));
  EXPECT_FALSE(warning.empty());
  std::ofstream(root_ / "ext/eos/actions/bad.v1.json") << "{bad";
  EXPECT_FALSE(catalog.LoadContracts(root_.string(), &warning));
}

TEST_F(CatalogTest, DiscoveryReplacesPrimitiveBindingsAuthoritatively) {
  Catalog catalog;
  catalog.Observe(
      "Discover",
      json{{"devices",
            {{{"id", "arm-0"}, {"primitives", {{{"name", "arm.movej"}, {"version", "1.0.0"}}}}}}}});
  EXPECT_EQ(catalog.DevicesFor("arm.movej", "1.0.0"), (std::vector<std::string>{"arm-0"}));
  catalog.Observe("Discover",
                  json{{"devices",
                        {{{"id", "base-0"},
                          {"primitives", {{{"name", "base.velocity"}, {"version", "1.0.0"}}}}}}}});
  EXPECT_TRUE(catalog.DevicesFor("arm.movej", "1.0.0").empty());
  EXPECT_EQ(catalog.DevicesFor("base.velocity", "1.0.0"), (std::vector<std::string>{"base-0"}));
}

TEST_F(CatalogTest, DiscoveryIncludesRobotScopedPrimitiveBindings) {
  Catalog catalog;
  catalog.Observe("Discover",
                  json{{"robot",
                        {{"name", "demo"},
                         {"primitives",
                          {{{"name", "robot.stop"}, {"version", "1.0.0"}},
                           {{"name", "safety.estop"}, {"version", "1.0.0"}}}}}},
                       {"devices",
                        {{{"id", "base-0"},
                          {"class", "base"},
                          {"primitives", {{{"name", "base.posture"}, {"version", "1.0.0"}}}}}}}});
  EXPECT_EQ(catalog.DevicesFor("robot.stop", "1.0.0"), (std::vector<std::string>{"-"}));
  EXPECT_EQ(catalog.DevicesFor("safety.estop", "1.0.0"), (std::vector<std::string>{"-"}));
  EXPECT_EQ(catalog.DevicesFor("base.posture", "1.0.0"), (std::vector<std::string>{"base-0"}));
  EXPECT_EQ(catalog.PrimitiveCount(), 3u);

  catalog.Observe("Discover", json{{"robot", {{"name", "demo"}, {"primitives", json::array()}}},
                                   {"devices", json::array()}});
  EXPECT_TRUE(catalog.DevicesFor("robot.stop", "1.0.0").empty());
  EXPECT_TRUE(catalog.DevicesFor("base.posture", "1.0.0").empty());
  EXPECT_EQ(catalog.PrimitiveCount(), 0u);
}

TEST_F(CatalogTest, RobotOnlyDiscoveryIsAuthoritative) {
  Catalog catalog;
  catalog.Observe(
      "Discover",
      json{{"robot",
            {{"name", "solo"}, {"primitives", {{{"name", "robot.stop"}, {"version", "1.0.0"}}}}}}});
  EXPECT_EQ(catalog.Robots(), (std::vector<std::string>{"solo"}));
  EXPECT_EQ(catalog.DevicesFor("robot.stop", "1.0.0"), (std::vector<std::string>{"-"}));
}

TEST_F(CatalogTest, DiscoveryRestrictsStaticContractsAndBindingHints) {
  std::ofstream(root_ / "ext/eos/actions/base.v1.json")
      << R"({"x-island-primitive-version":"1.0.0","properties":{"type":{"const":"base.navigate"},"reference":{"enum":["relative","absolute"]},"frame_id":{"type":"string"}}})";
  std::ofstream(root_ / "ext/eos/actions/arm.v1.json")
      << R"({"x-island-primitive-version":"1.0.0","properties":{"type":{"const":"arm.init"}}})";
  Catalog catalog;
  std::string warning;
  ASSERT_TRUE(catalog.LoadContracts(root_.string(), &warning)) << warning;
  EXPECT_EQ(catalog.Primitives(), (std::vector<std::string>{"arm.init", "base.navigate"}));

  catalog.Observe("Discover", json{{"devices",
                                    {{{"id", "base-0"},
                                      {"primitives",
                                       {{{"name", "base.navigate"},
                                         {"version", "1.0.0"},
                                         {"references", json::array({"absolute"})},
                                         {"absoluteFrame", "map"}}}}}}}});
  EXPECT_EQ(catalog.Primitives(), (std::vector<std::string>{"base.navigate"}));
  EXPECT_TRUE(catalog.PrimitiveVersionsFor("arm.init").empty());
  const auto arguments = catalog.ArgumentsFor("base.navigate", "1.0.0", "base-0");
  EXPECT_EQ(arguments.at("reference").values, (std::vector<std::string>{"absolute"}));
  EXPECT_EQ(arguments.at("frame_id").values, (std::vector<std::string>{"map"}));
}

TEST_F(CatalogTest, RemoteParameterSchemasOverrideLocalContractsPerDevice) {
  std::ofstream(root_ / "ext/eos/actions/base.v1.json")
      << R"({"x-island-primitive-version":"1.0.0","properties":{"type":{"const":"base.posture"},"posture":{"enum":["local"]}}})";
  Catalog catalog;
  std::string warning;
  ASSERT_TRUE(catalog.LoadContracts(root_.string(), &warning)) << warning;
  catalog.Observe(
      "Discover",
      json{{"devices",
            {{{"id", "base-0"},
              {"primitives",
               {{{"name", "base.posture"},
                 {"version", "1.0.0"},
                 {"parameters",
                  {{"type", "object"},
                   {"required", {"posture"}},
                   {"properties",
                    {{"posture", {{"type", "string"}, {"enum", {"stand", "lie"}}}},
                     {"duration", {{"type", "number"}, {"default", 1.5}}}}}}}}}}},
             {{"id", "base-1"},
              {"primitives",
               {{{"name", "base.posture"},
                 {"version", "1.0.0"},
                 {"parameters",
                  {{"type", "object"},
                   {"properties", {{"posture", {{"const", "kneel"}}}}}}}}}}}}}});

  const auto first = catalog.ArgumentsFor("base.posture", "1.0.0", "base-0");
  ASSERT_EQ(first.size(), 2u);
  EXPECT_TRUE(first.at("posture").required);
  EXPECT_EQ(first.at("posture").values, (std::vector<std::string>{"stand", "lie"}));
  EXPECT_TRUE(first.at("duration").has_default);
  const auto second = catalog.ArgumentsFor("base.posture", "1.0.0", "base-1");
  ASSERT_EQ(second.size(), 1u);
  EXPECT_EQ(second.at("posture").values, (std::vector<std::string>{"kneel"}));
}

TEST_F(CatalogTest, EmptyRemoteParameterSchemaIsPresentAndAuthoritative) {
  Catalog catalog;
  const json parameters = {{"type", "object"},
                           {"properties", json::object()},
                           {"additionalProperties", false}};
  const json binding = {
      {"name", "robot.stop"}, {"version", "1.0.0"}, {"parameters", parameters}};
  catalog.Observe("Discover",
                  {{"robot", {{"name", "demo"}, {"primitives", json::array({binding})}}}});

  EXPECT_TRUE(catalog.HasPrimitiveParameterSchema("robot.stop", "1.0.0"));
  const auto arguments = catalog.ArgumentsFor("robot.stop", "1.0.0");
  EXPECT_TRUE(arguments.empty());
  std::string error;
  EXPECT_TRUE(catalog.Validate(arguments, json::object(), &error)) << error;
  EXPECT_FALSE(catalog.Validate(arguments, {{"unexpected", true}}, &error));
  EXPECT_FALSE(catalog.HasPrimitiveParameterSchema("safety.estop", "1.0.0"));
}

}  // namespace
}  // namespace eos::ext::repl

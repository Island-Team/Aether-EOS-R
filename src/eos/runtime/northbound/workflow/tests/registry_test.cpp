#include "src/eos/runtime/northbound/workflow/registry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace eos::runtime::northbound {
namespace {

TEST(WorkflowTemplateRegistryTest, PublicCatalogStartsEmpty) {
  WorkflowTemplateRegistry registry(EOS_WORKFLOW_TEMPLATES_ROOT);
  EXPECT_EQ(registry.size(), 0u);
  EXPECT_TRUE(registry.List().empty());
  EXPECT_FALSE(registry.Get("missing").has_value());
}

TEST(WorkflowTemplateRegistryTest, LoadsTemplatesByFilename) {
  const auto directory = std::filesystem::temp_directory_path() /
                         ("eos-workflow-registry-valid-" +
                          std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
  std::filesystem::create_directories(directory);
  const auto template_path = directory / "example.json";
  {
    std::ofstream output(template_path);
    output << R"({"workflowId":"example-workflow","actions":[{"actionId":"stop",)"
              R"("primitive":{"name":"robot.stop","version":"1.0.0","deviceId":""},)"
              R"("parameters":{}}]})";
  }
  WorkflowTemplateRegistry registry(directory);
  ASSERT_EQ(registry.size(), 1u);
  const auto descriptor = registry.Get("example");
  ASSERT_TRUE(descriptor.has_value());
  EXPECT_EQ(descriptor->name(), "example");
  EXPECT_EQ(descriptor->definition().workflow_id(), "example-workflow");
  ASSERT_EQ(descriptor->definition().actions_size(), 1);
  EXPECT_EQ(descriptor->definition().actions(0).primitive().name(), "robot.stop");
  EXPECT_FALSE(registry.Get("missing").has_value());
  std::filesystem::remove_all(directory);
}

TEST(WorkflowTemplateRegistryTest, RejectsInvalidDependencyGraphsAtStartup) {
  const auto directory = std::filesystem::temp_directory_path() /
                         ("eos-workflow-registry-" +
                          std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
  std::filesystem::create_directories(directory);
  const auto template_path = directory / "cycle.json";
  {
    std::ofstream output(template_path);
    output << R"({"workflowId":"cycle","actions":[)"
              R"({"actionId":"a","skill":{"name":"one","version":"1"},"parameters":{},"dependsOn":["b"]},)"
              R"({"actionId":"b","skill":{"name":"two","version":"1"},"parameters":{},"dependsOn":["a"]})"
              R"(]})";
  }
  ASSERT_TRUE(std::filesystem::is_regular_file(template_path));
  EXPECT_THROW({ WorkflowTemplateRegistry registry(directory); }, std::runtime_error);
  std::filesystem::remove(template_path);
  std::filesystem::remove(directory);
}

TEST(WorkflowTemplateRegistryTest, LoadsChoiceMonitorControlAndNoopActions) {
  const auto directory = std::filesystem::temp_directory_path() /
                         ("eos-workflow-registry-extended-" +
                          std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
  std::filesystem::create_directories(directory);
  const auto template_path = directory / "room.json";
  {
    std::ofstream output(template_path);
    output << R"({
      "workflowId":"room",
      "actions":[
        {"actionId":"observe","skill":{"name":"observe_human","version":"2.2.0"},
          "parameters":{},"executionMode":"ACTION_EXECUTION_MODE_MONITOR",
          "monitorLifetime":"MONITOR_LIFETIME_EXPLICIT","continueOnFailure":true},
        {"actionId":"empty","noop":{},"parameters":{},"dependsOn":["observe"],
         "choiceGroup":"room-result","when":{"factKey":"human/observation","equals":"none",
         "sourceActionId":"observe"}},
        {"actionId":"stop","control":{"targetActionId":"observe",
         "operation":"ACTION_CONTROL_OPERATION_TERMINATE"},
         "parameters":{},"dependsOn":["empty"]}
      ]
    })";
  }
  WorkflowTemplateRegistry registry(directory);
  const auto descriptor = registry.Get("room");
  ASSERT_TRUE(descriptor.has_value());
  ASSERT_EQ(descriptor->definition().actions_size(), 3);
  EXPECT_EQ(descriptor->definition().actions(0).execution_mode(),
            island::eos::v1::ACTION_EXECUTION_MODE_MONITOR);
  EXPECT_TRUE(descriptor->definition().actions(0).continue_on_failure());
  EXPECT_EQ(descriptor->definition().actions(1).when().source_action_id(), "observe");
  EXPECT_TRUE(descriptor->definition().actions(1).has_noop());
  EXPECT_EQ(descriptor->definition().actions(2).control().operation(),
            island::eos::v1::ACTION_CONTROL_OPERATION_TERMINATE);
  std::filesystem::remove_all(directory);
}

TEST(WorkflowTemplateRegistryTest, RejectsUnknownConditionSourceAction) {
  const auto directory = std::filesystem::temp_directory_path() /
                         ("eos-workflow-registry-bad-source-" +
                          std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
  std::filesystem::create_directories(directory);
  const auto template_path = directory / "bad.json";
  {
    std::ofstream output(template_path);
    output << R"({"workflowId":"bad","actions":[
      {"actionId":"branch","noop":{},"parameters":{},"choiceGroup":"choice",
       "when":{"factKey":"human/observation","equals":"none","sourceActionId":"missing"}}
    ]})";
  }
  EXPECT_THROW({ WorkflowTemplateRegistry registry(directory); }, std::runtime_error);
  std::filesystem::remove_all(directory);
}

TEST(WorkflowTemplateRegistryTest, LoadsResourceConditionsAndRejectsConflicts) {
  const auto directory = std::filesystem::temp_directory_path() /
                         ("eos-workflow-registry-resource-" +
                          std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
  std::filesystem::create_directories(directory);
  {
    std::ofstream output(directory / "good.json");
    output << R"({"workflowId":"good","actions":[)"
               R"({"actionId":"assist","skill":{"name":"interact_waving_owner","version":"1.0.0"},)"
              R"("parameters":{},"when":{"resourceUri":"robots/demo-0/detection/state",)"
              R"("detectionKind":"human","detectionValuePointer":"/attributes/action",)"
              R"("equals":"waving","maxAge":"5s"}})"
              R"(]})";
  }
  WorkflowTemplateRegistry registry(directory);
  const auto descriptor = registry.Get("good");
  ASSERT_TRUE(descriptor.has_value());
  ASSERT_EQ(descriptor->definition().actions_size(), 1);
  EXPECT_EQ(descriptor->definition().actions(0).when().resource_uri(),
            "robots/demo-0/detection/state");
  EXPECT_EQ(descriptor->definition().actions(0).when().detection_kind(), "human");
  EXPECT_EQ(descriptor->definition().actions(0).when().detection_value_pointer(),
            "/attributes/action");
  EXPECT_EQ(descriptor->definition().actions(0).when().equals().string_value(), "waving");

  const std::string invalid[] = {
      R"({"workflowId":"conflict","actions":[{"actionId":"a","noop":{},"parameters":{},
         "when":{"resourceUri":"robots/r/detection/state","detectionKind":"human",
         "detectionValuePointer":"/attributes/action","factKey":"gate","equals":"waving"}}]})",
      R"({"workflowId":"bad-action-value","actions":[{"actionId":"a","noop":{},"parameters":{},
         "when":{"resourceUri":"robots/r/detection/state","detectionKind":"person",
         "detectionValuePointer":"/attributes/action","equals":"waving"}}]})",
      R"({"workflowId":"missing-action-value","actions":[{"actionId":"a","noop":{},
         "parameters":{},"when":{"resourceUri":"robots/r/detection/state",
         "detectionKind":"human","equals":"waving"}}]})",
      R"({"workflowId":"source","actions":[{"actionId":"a","noop":{},"parameters":{},
         "when":{"resourceUri":"robots/r/detection/state","detectionKind":"human",
         "detectionValuePointer":"/attributes/action","equals":"waving",
         "sourceActionId":"a"}}]})",
      R"({"workflowId":"private","actions":[{"actionId":"a","noop":{},"parameters":{},
         "when":{"resourceUri":"robots/r/detection/state","detectionKind":"human",
         "detectionValuePointer":"/keypoints","equals":[]}}]})"};
  for (const auto& index : {0, 1, 2, 3, 4}) {
    const auto bad = directory / ("bad-" + std::to_string(index) + ".json");
    {
      std::ofstream output(bad);
      output << invalid[index];
    }
    EXPECT_THROW({ WorkflowTemplateRegistry bad_registry(directory); }, std::runtime_error);
    std::filesystem::remove(bad);
  }
  std::filesystem::remove_all(directory);
}

}  // namespace
}  // namespace eos::runtime::northbound

#include "adapters/eos-cxx/ext/repl/completion.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace eos::ext::repl {
namespace {

using json = nlohmann::json;

TEST(CompletionTest, CompletesCommandsAndFixedSyntax) {
  Catalog catalog;
  CompletionEngine completion(&catalog);
  int context = 0;
  EXPECT_EQ(completion.Complete("ref", &context), (std::vector<std::string>{"refresh"}));
  EXPECT_EQ(context, 3);
  EXPECT_EQ(completion.Complete("audit", &context), (std::vector<std::string>{"audit-status"}));
  EXPECT_EQ(completion.Complete("auto ", &context), (std::vector<std::string>{"base"}));
  EXPECT_EQ(completion.Complete("set full o", &context), (std::vector<std::string>{"on", "off"}));
  EXPECT_EQ(completion.Complete("set b", &context), (std::vector<std::string>{"brief"}));
  EXPECT_EQ(completion.Complete("set brief o", &context), (std::vector<std::string>{"on", "off"}));
  EXPECT_EQ(completion.Complete("skill", &context), (std::vector<std::string>{"skill", "skills"}));
  EXPECT_EQ(completion.Complete("pkg lo", &context), (std::vector<std::string>{"lock"}));
  EXPECT_TRUE(completion.Complete("pkg us", &context).empty());
  EXPECT_EQ(completion.Complete("pkg lock ", &context),
            (std::vector<std::string>{"add", "remove", "show", "verify"}));
  EXPECT_EQ(completion.Complete("pkg skills ", &context),
            (std::vector<std::string>{"path", "sync"}));
}

TEST(CompletionTest, CompletesAdmittedCapabilityNames) {
  Catalog catalog;
  catalog.Observe("ListCapabilities",
                  {{"capabilities", {{{"name", "hardware/demo/base/default/base.navigate@1.0.0"},
                                      {"available", true}}}}});
  CompletionEngine completion(&catalog);
  int context = 0;
  EXPECT_EQ(completion.Complete("call hardware/demo/base", &context),
            (std::vector<std::string>{"hardware/demo/base/default/base.navigate@1.0.0"}));
  EXPECT_EQ(completion.Complete("call hardware/demo/base/default/base.navigate@1.0.0 --j",
                                &context),
            (std::vector<std::string>{"--json"}));
  EXPECT_NE(completion.Hint("operation ", &context).find("operation-id"), std::string::npos);
}

TEST(CompletionTest, EmptyTabListsEveryCommandAndAlias) {
  Catalog catalog;
  CompletionEngine completion(&catalog);
  int context = -1;
  EXPECT_EQ(completion.Complete("", &context), (std::vector<std::string>{"?",
                                                                         "audit-status",
                                                                         "auto",
                                                                         "backend",
                                                                         "capabilities",
                                                                         "call",
                                                                         "cancel-operation",
                                                                         "capture",
                                                                         "connect",
                                                                         "describe",
                                                                         "detect-faces",
                                                                         "detect-humans",
                                                                         "disconnect",
                                                                         "discover",
                                                                         "eos",
                                                                         "estimate-human-pose",
                                                                         "execution",
                                                                         "executions",
                                                                         "exit",
                                                                         "help",
                                                                         "history",
                                                                         "infer-vad",
                                                                         "journalctl",
                                                                         "kill",
                                                                         "leave",
                                                                         "ls",
                                                                         "observe-objects",
                                                                         "operation",
                                                                         "perception-roles",
                                                                         "pkg",
                                                                         "pkill",
                                                                         "ps",
                                                                         "quit",
                                                                         "read",
                                                                         "refresh",
                                                                         "resources",
                                                                         "runtime",
                                                                         "set",
                                                                         "shutdown",
                                                                         "sig",
                                                                         "ss",
                                                                         "skill",
                                                                         "skills",
                                                                         "start",
                                                                         "state",
                                                                         "status",
                                                                         "top",
                                                                         "watch",
                                                                         "watch-operation",
                                                                         "watch-execution",
                                                                         "watch-state",
                                                                         "waypoint",
                                                                         "waypoints",
                                                                         "workflow",
                                                                         "workflows"}));
  EXPECT_EQ(context, 0);
  EXPECT_EQ(completion.Complete("resources ", &context), (std::vector<std::string>{"list"}));
  EXPECT_EQ(completion.Complete("eos re", &context), (std::vector<std::string>{"restart"}));
  EXPECT_EQ(completion.Complete("backend st", &context),
            (std::vector<std::string>{"start", "stop", "status"}));
  EXPECT_EQ(completion.Complete("pkg in", &context), (std::vector<std::string>{"info", "install"}));
  EXPECT_EQ(completion.Complete("status ", &context),
            (std::vector<std::string>{"arm", "base", "gripper"}));
  EXPECT_EQ(completion.Complete("help sk", &context),
            (std::vector<std::string>{"skill", "skills"}));
}

TEST(CompletionTest, UsesRuntimeMetadataForTargets) {
  Catalog catalog;
  catalog.Observe("Discover", json::parse(R"({"robot":{"name":"demo"},"devices":[
    {"id":"camera-0","class":"camera","primitives":[]},
    {"id":"gimbal-0","class":"gimbal","primitives":[]},
    {"id":"base-0","class":"base","primitives":[{"name":"base.velocity","version":"1.0.0"}]},
    {"id":"base-1","primitives":[{"name":"base.velocity","version":"1.0.0"}]}
  ]})"));
  catalog.Observe("ListResources", json::array({{{"uri", "robots/r/status"}}}));
  CompletionEngine completion(&catalog);
  int context = 0;
  EXPECT_TRUE(completion.Complete("call base.velocity ", &context).empty());
  EXPECT_TRUE(completion.Complete("call base.velocity 1.0.0 b", &context).empty());
  EXPECT_EQ(completion.Complete("read robots", &context),
            (std::vector<std::string>{"robots/r/status"}));
  EXPECT_EQ(completion.Complete("history robots", &context),
            (std::vector<std::string>{"robots/r/status"}));
  EXPECT_EQ(
      completion.Complete("history robots/r/status --", &context),
      (std::vector<std::string>{"--after", "--before", "--nearest", "--range", "--sequence"}));
  EXPECT_EQ(completion.Complete("history robots/r/status --range 1 4 --", &context),
            (std::vector<std::string>{"--limit"}));
  EXPECT_EQ(completion.Complete("capture d", &context), (std::vector<std::string>{"demo"}));
  EXPECT_EQ(completion.Complete("capture demo cam", &context),
            (std::vector<std::string>{"camera-0"}));
  EXPECT_EQ(completion.Complete("capture demo camera-0 ", &context),
            (std::vector<std::string>{"1", "2"}));
  EXPECT_EQ(completion.Complete("status base ", &context), (std::vector<std::string>{"demo"}));
  EXPECT_EQ(completion.Complete("waypoints d", &context), (std::vector<std::string>{"demo"}));
}

TEST(CompletionTest, CompletesNavigationPointCommandsRobotsAndObservedNames) {
  Catalog catalog;
  catalog.Observe("Discover", json{{"robot", {{"name", "robot-a"}}},
                                    {"devices", json::array()}});
  catalog.Observe(
      "ListResources",
      json::array({{{"uri", "robots/robot-a/navigation/waypoints/Entry_in"}},
                   {{"uri", "robots/robot-a/navigation/waypoints/Entry_out"}},
                   {{"uri", "robots/other/navigation/waypoints/Elsewhere"}}}));
  CompletionEngine completion(&catalog);
  int context = 0;
  EXPECT_EQ(completion.Complete("waypoint ", &context),
            (std::vector<std::string>{"delete", "get", "put", "reload"}));
  EXPECT_EQ(completion.Complete("waypoint get rob", &context),
            (std::vector<std::string>{"robot-a"}));
  EXPECT_EQ(completion.Complete("waypoint get robot-a Entry_", &context),
            (std::vector<std::string>{"Entry_in", "Entry_out"}));
  EXPECT_EQ(completion.Complete("waypoint delete robot-a Entry_o", &context),
            (std::vector<std::string>{"Entry_out"}));
  EXPECT_NE(completion.Hint("waypoint put ", &context).find("<robot-name>"),
            std::string::npos);
}

TEST(CompletionTest, CompletesTypedPerceptionJsonInput) {
  Catalog catalog;
  CompletionEngine completion(&catalog);
  int context = 0;
  EXPECT_EQ(completion.Complete("infer-vad --", &context), (std::vector<std::string>{"--json"}));
  EXPECT_EQ(completion.Complete("detect-humans --", &context),
            (std::vector<std::string>{"--json"}));
  EXPECT_NE(completion.Hint("estimate-human-pose ", &context).find("--json"), std::string::npos);
}

TEST(CompletionTest, LegacyRobotPrimitiveNamesAreNotCompletionTargets) {
  Catalog catalog;
  catalog.Observe("Discover", json{{"robot",
                                    {{"name", "demo"},
                                     {"primitives",
                                      {{{"name", "robot.stop"}, {"version", "1.0.0"}},
                                       {{"name", "safety.estop"}, {"version", "1.0.0"}}}}}},
                                   {"devices", json::array()}});
  CompletionEngine completion(&catalog);
  int context = 0;
  EXPECT_TRUE(completion.Complete("call robot.stop 1.0.0 ", &context).empty());
  EXPECT_TRUE(completion.Complete("call safety.estop 1.0.0 ", &context).empty());

  Catalog undiscovered;
  CompletionEngine offline(&undiscovered);
  EXPECT_TRUE(offline.Complete("call safety.estop 1.0.0 ", &context).empty());
}

TEST(CompletionTest, WorksOfflineAndSuggestsJson) {
  Catalog catalog;
  CompletionEngine completion(&catalog);
  int context = 0;
  EXPECT_TRUE(completion.Complete("read r", &context).empty());
  EXPECT_TRUE(completion.Complete("call unknown 9.9.9 device --j", &context).empty());
  EXPECT_FALSE(completion.Hint("call ", &context).empty());
}

TEST(CompletionTest, LegacyPrimitiveSchemaCompletionIsRetired) {
  Catalog catalog;
  std::string warning;
  ASSERT_TRUE(catalog.LoadContracts(EOS_REPL_CONTRACTS_ROOT, &warning)) << warning;
  catalog.Observe(
      "Discover",
      json{{"devices",
            {{{"id", "base-0"},
              {"primitives",
               {{{"name", "base.velocity"},
                 {"version", "1.0.0"},
                 {"limits",
                  {{"linear_x_m_s",
                    {{"minimum", -1.0}, {"maximum", 1.0}, {"minimumExclusive", true}}}}}},
                {{"name", "base.posture"}, {"version", "1.0.0"}},
                {{"name", "base.navigate"},
                 {"version", "1.0.0"},
                 {"references", json::array({"absolute"})},
                 {"absoluteFrame", "map"}}}}}}}});
  CompletionEngine completion(&catalog);
  int context = 0;
  EXPECT_TRUE(completion.Complete("call base.posture 1.0.0 base-0 pos", &context).empty());
  EXPECT_TRUE(completion.Complete("call base.posture 1.0.0 base-0 posture=s", &context).empty());
  EXPECT_TRUE(completion.Complete("call arm.init 1.0.0 ", &context).empty());
  EXPECT_TRUE(completion.Complete("call base.navigate 1.0.0 base-0 reference=", &context).empty());
  EXPECT_TRUE(completion.Complete("call base.navigate 1.0.0 base-0 frame_id=", &context).empty());
}

TEST(CompletionTest, CompletesRemoteSkillsAndPromptModes) {
  Catalog catalog;
  catalog.Observe(
      "ListSkills",
      nlohmann::json{
          {"skills",
           {{{"reference", {{"name", "demo.move"}, {"version", "1.0.0"}}},
             {"parameters",
              {{"properties",
                {{"safe", {{"type", "boolean"}}}, {"mode", {{"enum", {"slow", "fast"}}}}}}}}}}}});
  catalog.Observe(
      "ListExecutions",
      {{"executions",
        nlohmann::json::array({{{"executionId", "01a0-full-execution-id"},
                                {"reference", {{"name", "demo.move"}, {"version", "1.0.0"}}},
                                {"status", "RUNNING"}}})}});
  CompletionEngine completion(&catalog);
  int context = 0;
  EXPECT_EQ(completion.Complete("start demo.", &context), (std::vector<std::string>{"demo.move"}));
  EXPECT_EQ(completion.Complete("start demo.move ", &context),
            (std::vector<std::string>{"mode= safe="}));
  EXPECT_EQ(completion.Complete("start demo.move 1.0.0 ", &context),
            (std::vector<std::string>{"mode= safe="}));
  EXPECT_EQ(context, 0);
  EXPECT_EQ(completion.Complete("start demo.move 1.0.0 --p", &context),
            (std::vector<std::string>{"--prompt"}));
  EXPECT_EQ(completion.Complete("start demo.move 1.0.0 mode=f", &context),
            (std::vector<std::string>{"mode=fast"}));
  EXPECT_EQ(completion.Complete("start demo.move 1.0.0 mode=fast ", &context),
            (std::vector<std::string>{"safe="}));
  EXPECT_EQ(completion.Complete("ps demo.", &context), (std::vector<std::string>{"demo.move"}));
  EXPECT_EQ(completion.Complete("ps demo.move ", &context),
            (std::vector<std::string>{"1.0.0", "--all", "--status", "--watch"}));
  EXPECT_EQ(completion.Complete("ps demo.move 1.0.0 --status R", &context),
            (std::vector<std::string>{"RUNNING", "REJECTED"}));
  EXPECT_EQ(completion.Complete("ps --status QUEUED,R", &context),
            (std::vector<std::string>{"QUEUED,RUNNING", "QUEUED,REJECTED"}));
  for (const std::string command :
       {"execution", "kill", "state", "watch-execution", "watch-state", "journalctl"}) {
    EXPECT_EQ(completion.Complete(command + " 01a0-", &context),
              (std::vector<std::string>{"01a0-full-execution-id"}));
  }
  EXPECT_EQ(completion.Complete("sig ", &context), (std::vector<std::string>{"cancel"}));
  EXPECT_EQ(completion.Complete("sig cancel 01a0-", &context),
            (std::vector<std::string>{"01a0-full-execution-id"}));
}

TEST(CompletionTest, CompletesDeclaredSignalNamesActiveExecutionsAndPayloads) {
  Catalog catalog;
  catalog.Observe(
      "ListSkills",
      json{{"skills",
            {{{"reference", {{"name", "demo.move"}, {"version", "1.0.0"}}},
              {"acceptedSignals",
               {{{"name", "adjust"},
                 {"payloadSchema", {{"properties", {{"mode", {{"enum", {"slow", "fast"}}}}}}}}},
                {{"name", "skill.command"},
                 {"payloadSchema",
                  {{"properties",
                    {{"command", {{"enum", {"pause", "resume"}}}},
                     {"source", {{"enum", {"operator"}}}}}}}}}}}}}}});
  catalog.Observe("ListExecutions",
                  {{"executions",
                    {{{"executionId", "active-full-id"},
                      {"status", "EXECUTION_STATUS_RUNNING"},
                      {"reference", {{"name", "demo.move"}, {"version", "1.0.0"}}}},
                     {{"executionId", "finished-full-id"},
                      {"status", "EXECUTION_STATUS_SUCCEEDED"},
                      {"reference", {{"name", "demo.move"}, {"version", "1.0.0"}}}}}}});
  CompletionEngine completion(&catalog);
  int context = 0;
  EXPECT_EQ(completion.Complete("sig a", &context), (std::vector<std::string>{"adjust"}));
  EXPECT_EQ(completion.Complete("sig adjust active", &context),
            (std::vector<std::string>{"active-full-id"}));
  EXPECT_EQ(completion.Complete("sig adjust active-full-id mode=f", &context),
            (std::vector<std::string>{"mode=fast"}));
  EXPECT_TRUE(completion.Complete("sig pause active-full-id source=", &context).empty());
}

TEST(CompletionTest, CompletesRegisteredWorkflowTemplates) {
  Catalog catalog;
  catalog.Observe("ListWorkflowTemplates", json{{"templates", {{{"name", "register_host"}}}}});
  CompletionEngine completion(&catalog);
  int context = 0;
  EXPECT_EQ(completion.Complete("workflow r", &context), (std::vector<std::string>{"run"}));
  EXPECT_EQ(completion.Complete("workflow w", &context), (std::vector<std::string>{"watch"}));
  EXPECT_EQ(completion.Complete("workflow run reg", &context),
            (std::vector<std::string>{"register_host"}));
}

TEST(CompletionTest, TabFillsEveryObservedSkillParameter) {
  Catalog catalog;
  catalog.Observe(
      "Discover",
      json{{"robot", {{"name", "robot-a"}}},
           {"devices",
            {{{"id", "gimbal-0"}, {"class", "gimbal"}, {"primitives", json::array()}},
             {{"id", "speaker-0"}, {"class", "speaker"}, {"primitives", json::array()}},
             {{"id", "speaker-1"}, {"class", "speaker"}, {"primitives", json::array()}}}}});
  catalog.Observe("GetSkill",
                  json{{"reference", {{"name", "observe_human"}, {"version", "2.1.0"}}},
                       {"parameters",
                        {{"type", "object"},
                         {"properties",
                          {{"robot_name", {{"type", "string"}}},
                           {"gimbal_device_id", {{"type", "string"}}},
                           {"speaker_device_id", {{"type", "string"}}},
                           {"minimum_confidence", {{"type", "number"}}}}},
                         {"required", {"robot_name", "gimbal_device_id", "speaker_device_id"}}}}});
  CompletionEngine completion(&catalog);
  int context = -1;
  EXPECT_EQ(completion.Complete("start observe_human 2.1.0 ", &context),
            (std::vector<std::string>{
                "gimbal_device_id=gimbal-0 minimum_confidence= robot_name=robot-a "
                "speaker_device_id="}));
  EXPECT_EQ(context, 0);
  EXPECT_EQ(
      completion.Complete(
          "start observe_human 2.1.0 gimbal_device_id=gimbal-0 robot_name=robot-a "
          "speaker_device_id=s",
          &context),
      (std::vector<std::string>{"speaker_device_id=speaker-0", "speaker_device_id=speaker-1"}));
}

}  // namespace
}  // namespace eos::ext::repl

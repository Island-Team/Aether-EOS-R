/// Formatter unit tests: readable interface views, optional base64 elision,
/// terminal colors, and stream notifications.

#include "src/eos/ext/repl/format.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <string>

namespace eos::ext::repl {
namespace {

using json = nlohmann::json;

std::string Blob(std::size_t size) { return std::string(size, 'x'); }

TEST(FormatTest, ElidesLongB64StringsWhenEnabled) {
  const json snapshot{{"image_b64", Blob(2000)}, {"state", {{"pcm_b64", Blob(300)}}}};
  const std::string out = FormatResult(snapshot, DisplayOptions{/*elide_b64=*/true});
  EXPECT_EQ(out.find("xxxx"), std::string::npos);  // no blob leaks through
  EXPECT_NE(out.find("<2000 chars b64>"), std::string::npos);
  EXPECT_NE(out.find("<300 chars b64>"), std::string::npos);
}

TEST(FormatTest, ElidesCanonicalCameraJpegBytes) {
  const json snapshot{{"type", "vision.snapshot"},
                      {"frames", json::array({{{"jpeg", Blob(500)}, {"widthPx", 1280}}})}};
  const std::string out = FormatResult(snapshot, DisplayOptions{/*elide_b64=*/true});
  EXPECT_EQ(out.find("xxxx"), std::string::npos);
  EXPECT_NE(out.find("<500 chars b64>"), std::string::npos);
  EXPECT_NE(out.find("Width (px): 1280"), std::string::npos);
}

TEST(FormatTest, ShortB64StringsSurviveElision) {
  const json payload{{"image_b64", nullptr}, {"pcm_b64", "c2hvcnQ="}};
  const std::string out = FormatResult(payload, DisplayOptions{/*elide_b64=*/true});
  EXPECT_NE(out.find("c2hvcnQ="), std::string::npos);
}

TEST(FormatTest, FullModeShowsEverythingByDefault) {
  const json snapshot{{"image_b64", Blob(200)}};
  const std::string out = FormatResult(snapshot, DisplayOptions{});
  EXPECT_NE(out.find(std::string(200, 'x')), std::string::npos);
}

TEST(FormatTest, NonB64LongStringsAreNeverElided) {
  const json fault{{"faults", json::array({"a very long diagnostic string that "
                                           "easily passes any blob threshold"})}};
  const std::string out = FormatResult(fault, DisplayOptions{/*elide_b64=*/true});
  EXPECT_NE(out.find("easily passes"), std::string::npos);
}

TEST(FormatTest, RenderReplyIncludesEveryAckField) {
  const json ack{{"type", "primitive.ack"},      {"sequence", 7},
                 {"primitive", "base.velocity"}, {"accepted", true},
                 {"reason_code", nullptr},       {"reason", nullptr},
                 {"command_id", "repl-1"},       {"result", {{"speed", 1.5}}}};
  const std::string out = RenderReply("CallPrimitive", ack, DisplayOptions{});
  EXPECT_EQ(out.find('{'), std::string::npos);
  EXPECT_NE(out.find("Primitive Release"), std::string::npos);
  EXPECT_NE(out.find("Accepted: yes"), std::string::npos);
  EXPECT_NE(out.find("Command ID: repl-1"), std::string::npos);
  EXPECT_NE(out.find("Reason: -"), std::string::npos);
  EXPECT_NE(out.find("Speed: 1.5"), std::string::npos);
  EXPECT_NE(out.find("Sequence: 7"), std::string::npos);
}

TEST(FormatTest, PrimitiveCancellationGetsReadableTitle) {
  const std::string out =
      RenderReply("CancelPrimitive", json{{"status", "cancelled"}}, DisplayOptions{});
  EXPECT_NE(out.find("Primitive Cancellation"), std::string::npos);
}

TEST(FormatTest, RendersSkillFailureReasonBeforeDiagnosticMessage) {
  const json execution{{"status", "EXECUTION_STATUS_FAILED"},
                       {"reason_code", "scan.motion_failed"},
                       {"message", "gimbal motion failed: capability unavailable"}};
  const std::string out = RenderReply("GetExecution", execution, DisplayOptions{});
  const auto reason = out.find("Reason Code: scan.motion_failed");
  const auto message = out.find("Message: gimbal motion failed: capability unavailable");
  ASSERT_NE(reason, std::string::npos);
  ASSERT_NE(message, std::string::npos);
  EXPECT_LT(reason, message);
}

TEST(FormatTest, RendersFilteredColoredExecutionPsSnapshot) {
  const json reply{{"executions",
                    json::array({{{"executionId", "running-execution-123456"},
                                  {"reference", {{"name", "observe_human"}, {"version", "1.0.0"}}},
                                  {"runtimeId", "default"},
                                  {"totalDuration", {{"seconds", "12"}, {"nanos", 500000000}}},
                                  {"currentActivity", {{"name", "tracking"}}},
                                  {"status", "EXECUTION_STATUS_RUNNING"}},
                                 {{"executionId", "finished"},
                                  {"reference", {{"name", "other"}, {"version", "1.0.0"}}},
                                  {"reasonCode", "completed"},
                                  {"status", "EXECUTION_STATUS_SUCCEEDED"}}})}};
  const std::string out = RenderExecutionPs(reply, json::object(), DisplayOptions{});
  EXPECT_NE(out.find("observe_human@1.0.0"), std::string::npos);
  EXPECT_NE(out.find("running-execution-123456"), std::string::npos);
  EXPECT_NE(out.find("RUNNING"), std::string::npos);
  EXPECT_NE(out.find("tracking"), std::string::npos);
  EXPECT_NE(out.find("12500ms"), std::string::npos);
  EXPECT_NE(out.find("other@1.0.0"), std::string::npos);
  EXPECT_NE(out.find("SUCCEEDED"), std::string::npos);
  EXPECT_NE(out.find("completed"), std::string::npos);
  const std::string colored =
      RenderExecutionPs(reply, {{"statuses", json::array({"RUNNING"})}},
                         DisplayOptions{/*elide_b64=*/false, /*color=*/true});
  EXPECT_NE(colored.find("\033[32m"), std::string::npos);
}

TEST(FormatTest, RendersProtoJsonDurationAndStringActivityInExecutionPs) {
  const json reply{{"executions",
                    json::array({{{"executionId", "running"},
                                  {"reference", {{"name", "observe_human"}, {"version", "1.0.0"}}},
                                  {"runtimeId", "builtin"},
                                  {"totalDuration", "12.500s"},
                                  {"currentActivity", "tracking"},
                                  {"status", "EXECUTION_STATUS_RUNNING"}}})}};

  const std::string out = RenderExecutionPs(reply, json::object(), DisplayOptions{});

  EXPECT_NE(out.find("tracking"), std::string::npos);
  EXPECT_NE(out.find("12500ms"), std::string::npos);
}

TEST(FormatTest, RenderReplyIncludesCompleteErrorObject) {
  const json reply{{"error",
                    {{"code", 3},
                     {"message", "unknown uri: nope"},
                     {"details", json::array({{{"field", "uri"}, {"valid", false}}})}}}};
  const std::string out = RenderReply("ReadResource", reply, DisplayOptions{});
  EXPECT_EQ(out.find('{'), std::string::npos);
  EXPECT_NE(out.find("Error\nCode: 3"), std::string::npos);
  EXPECT_NE(out.find("Message: unknown uri: nope"), std::string::npos);
  EXPECT_NE(out.find("Field: uri"), std::string::npos);
  EXPECT_NE(out.find("Valid: no"), std::string::npos);
}

TEST(FormatTest, RenderReplyHandlesEveryJsonValueType) {
  const json value{{"array", json::array({nullptr, true, false, -4, 1.25, "text"})},
                   {"object", {{"nested", json::array()}}}};
  const std::string out = RenderReply("Unknown", value, DisplayOptions{});
  EXPECT_NE(out.find("Array (6): -, yes, no, -4, 1.25, text"), std::string::npos);
  EXPECT_NE(out.find("Nested (0): (none)"), std::string::npos);
  EXPECT_EQ(RenderReply("Unknown", nullptr, DisplayOptions{}), "Response\n-");
  EXPECT_EQ(RenderReply("Unknown", true, DisplayOptions{}), "Response\nyes");
  EXPECT_EQ(RenderReply("Unknown", 42, DisplayOptions{}), "Response\n42");
  EXPECT_EQ(RenderReply("Unknown", "text", DisplayOptions{}), "Response\ntext");
}

TEST(FormatTest, ColorizesReadableViewByValueType) {
  const json value{{"active", true}, {"count", 2}, {"label", "ready"}, {"missing", nullptr}};
  const std::string out =
      RenderReply("GetStatus", value, DisplayOptions{/*elide_b64=*/false, /*color=*/true});

  EXPECT_NE(out.find("\033[1;36mRuntime Status\033[0m"), std::string::npos);
  EXPECT_NE(out.find("\033[1;34mActive\033[0m"), std::string::npos);
  EXPECT_NE(out.find("\033[32myes\033[0m"), std::string::npos);
  EXPECT_NE(out.find("\033[36m2\033[0m"), std::string::npos);
  EXPECT_NE(out.find("\033[32mready\033[0m"), std::string::npos);
  EXPECT_NE(out.find("\033[90m-\033[0m"), std::string::npos);
}

TEST(FormatTest, UsesKnownResourceHeadings) {
  EXPECT_NE(RenderReply("ReadResource", json{{"type", "world.snapshot"}}, DisplayOptions{})
                .find("World State\n"),
            std::string::npos);
  EXPECT_NE(RenderReply("ReadResource", json{{"type", "detection.snapshot"}}, DisplayOptions{})
                .find("Detection Snapshot\n"),
            std::string::npos);
  EXPECT_NE(RenderReply("ReadResource", json{{"type", "asr.snapshot"}}, DisplayOptions{})
                .find("Recognized Speech\n"),
            std::string::npos);
  EXPECT_NE(RenderReply("ReadResource", json{{"type", "arm.snapshot"}}, DisplayOptions{})
                .find("Arm State\n"),
            std::string::npos);
  EXPECT_NE(RenderReply("ReadResource", json{{"type", "vision.snapshot"}}, DisplayOptions{})
                .find("Vision Snapshot\n"),
            std::string::npos);
  EXPECT_NE(RenderReply("ReadResource", json{{"type", "robot.status"}}, DisplayOptions{})
                .find("Resource\n"),
            std::string::npos);
}

TEST(FormatTest, UsesNorthboundMethodHeadings) {
  EXPECT_EQ(RenderReply("Discover", json::object(), DisplayOptions{}), "EOS Discovery\n(none)");
  EXPECT_EQ(RenderReply("GetStatus", json::object(), DisplayOptions{}), "Runtime Status\n(none)");
  EXPECT_EQ(RenderReply("ListResources", json::array(), DisplayOptions{}),
            "Resources\nItems (0): (none)");
  EXPECT_EQ(RenderReply("CaptureFrames", json::object(), DisplayOptions{}),
            "Camera Frames\n(none)");
  EXPECT_EQ(RenderReply("QueryResourceHistory", json::object(), DisplayOptions{}),
            "Resource History\n(none)");
  EXPECT_EQ(RenderReply("InferVad", json::object(), DisplayOptions{}),
            "Voice Activity Detection\n(none)");
  EXPECT_EQ(RenderReply("ListSkills", json::object(), DisplayOptions{}), "Skills\n(none)");
  EXPECT_EQ(RenderReply("StartExecution", json::object(), DisplayOptions{}),
            "Skill Execution Started\n(none)");
  EXPECT_EQ(RenderReply("GetRuntimeSnapshot", json::object(), DisplayOptions{}),
            "Manager Runtime Snapshot\n(none)");
}

TEST(FormatTest, BriefModeCompactsResourceListings) {
  const json resources = json::array(
      {{{"class", "detection"}, {"uri", "robots/demo/detection/state"}},
       {{"class", "arm"}, {"uri", "robots/demo/arm/state"}}});
  DisplayOptions options;
  options.brief = true;
  EXPECT_EQ(RenderReply("ListResources", resources, options),
             "Resources\ndetection  robots/demo/detection/state\narm  robots/demo/arm/state");
}

TEST(FormatTest, BriefModeCompactsResourceHistoryAndNestedProducts) {
  const json history{
      {"uri", "robots/demo/detection/state"},
      {"products",
       json::array({{{"sequence", "7"},
                     {"observedAt", "2026-10-04T12:00:00Z"},
                     {"bootId", "boot-1"},
                     {"epoch", "epoch-1"},
                     {"value",
                      {{"type", "detection.snapshot"},
                       {"sequence", 9},
                       {"detections", json::array()}}}}})},
      {"retention",
       {{"firstAvailableSequence", "4"},
        {"latestSequence", "9"},
        {"evictedThroughSequence", "3"},
        {"closed", false}}},
      {"retentionGap", true},
      {"truncated", false}};
  DisplayOptions options;
  options.brief = true;
  EXPECT_EQ(RenderReply("QueryResourceHistory", history, options),
            "Resource History\nuri=robots/demo/detection/state products=1 first=4 latest=9 "
            "evicted-through=3 closed=false retention-gap=true truncated=false\n"
            "seq=7 observed-at=2026-10-04T12:00:00Z boot=boot-1 epoch=epoch-1 "
            "seq=9 detections=0");
}

TEST(FormatTest, BriefModeCompactsKnownResourceSnapshots) {
  DisplayOptions options;
  options.brief = true;
  const json detection{
      {"type", "detection.snapshot"},
      {"sequence", 8},
      {"source", "cam-0"},
      {"source_sequence", 7},
      {"source_generation", 12},
      {"models",
        {{"human", {{"status", "complete"}, {"sequence", 5}, {"source_generation", 11},
                     {"requested_generation", 11}}},
         {"pose", {{"status", "pending"}, {"sequence", 4}, {"source_generation", 10},
                    {"requested_generation", 12}}},
         {"action", {{"status", "pending"}, {"sequence", 4}, {"source_generation", 10},
                      {"requested_generation", 12}}},
         {"face", {{"status", "unavailable"}, {"sequence", 2}, {"source_generation", 10},
                    {"requested_generation", 10}}}}},
      {"detections",
       json::array({{{"id", "human:2"},
                      {"kind", "human"},
                      {"label", "person"},
                      {"confidence", 0.9},
                      {"attributes",
                       {{"name", "Ada"},
                        {"pose", "sit"},
                        {"posture_state", "sitting_on_furniture"},
                        {"activity", "sit"}}}}})}};
  const std::string detected = RenderReply("ReadResource", detection, options);
  EXPECT_NE(detected.find("source=cam-0 source-seq=7 source-gen=12 seq=8"), std::string::npos);
  EXPECT_NE(detected.find("human status=complete seq=5 source-gen=11 requested-gen=11"),
            std::string::npos);
  EXPECT_NE(detected.find("pose status=pending seq=4 source-gen=10 requested-gen=12"),
            std::string::npos);
  EXPECT_NE(detected.find("face status=unavailable seq=2 source-gen=10 requested-gen=10"),
            std::string::npos);
  EXPECT_NE(detected.find("detections=1"), std::string::npos);
  EXPECT_NE(detected.find("id=human:2 kind=human label=person confidence=0.9 name=Ada pose=sit "
                          "posture-state=sitting_on_furniture activity=sit"),
            std::string::npos);
  EXPECT_EQ(detected.find("Schema Version"), std::string::npos);

  const json arm{{"type", "arm.snapshot"},
                 {"part_id", "arm-0"},
                 {"sequence", 3},
                 {"pose_valid", true},
                 {"joint_positions_rad", json::array({0.0, nullptr, 1.5})}};
  EXPECT_EQ(RenderReply("ReadResource", arm, options),
            "Arm State\npart=arm-0 seq=3 pose-valid=true joints=[0.0,null,1.5]");

  const json asr{{"type", "asr.snapshot"},
                 {"sequence", 4},
                 {"text", "Stop."},
                 {"language", "en"},
                 {"confidence", 0.99}};
  EXPECT_EQ(RenderReply("ReadResource", asr, options),
            "Recognized Speech\ntext=Stop. language=en confidence=0.99 seq=4");
}

TEST(FormatTest, BriefModeCompactsDeviceVadAndExecutionShapes) {
  DisplayOptions options;
  options.brief = true;
  EXPECT_EQ(FormatResult(json{{"device_class", "base"},
                              {"operational_state",
                               {{"controller_state", "active"}, {"motors_enabled", true}}}},
                         options),
            "class=base state=active motors=true");
  EXPECT_EQ(FormatResult(json{{"type", "device.snapshot"},
                              {"device_id", "camera-0"},
                              {"class", "camera"},
                              {"state", "ready"}},
                         options),
            "device=camera-0 class=camera state=ready");
  EXPECT_EQ(FormatResult(json{{"speech", true},
                              {"probability", 0.8},
                              {"segments", json::array({{{"start_s", 0}, {"end_s", 1}}})}},
                         options),
            "speech=true probability=0.8 segments=1");
  EXPECT_EQ(FormatResult(json{{"executionId", "exec-1"},
                              {"reference", {{"name", "inspect"}, {"version", "1.0.0"}}},
                              {"status", "EXECUTION_STATUS_RUNNING"},
                              {"currentActivity", {{"name", "tracking"}}}},
                         options),
            "skill=inspect@1.0.0 execution=exec-1 state=EXECUTION_STATUS_RUNNING "
            "activity=tracking");
}

TEST(FormatTest, BriefModeLeavesUnknownPayloadsAndNormalModeUnchanged) {
  const json payload{{"type", "custom.snapshot"}, {"nested", {{"value", 2}}}};
  DisplayOptions brief;
  brief.brief = true;
  EXPECT_EQ(FormatResult(payload, brief), FormatResult(payload, DisplayOptions{}));

  const json detection{{"type", "detection.snapshot"}, {"detections", json::array()}};
  const std::string normal = RenderReply("ReadResource", detection, DisplayOptions{});
  EXPECT_NE(normal.find("Type: detection.snapshot"), std::string::npos);
  EXPECT_NE(normal.find("Detections (0): (none)"), std::string::npos);
}

TEST(FormatTest, ExecutionStateFormattingComposesWithBriefPayloads) {
  const json state{{"executionId", "exec-1"},
                   {"revision", "4"},
                   {"publishedAt", "2026-10-04T12:00:00Z"},
                   {"payload",
                    {{"type", "detection.snapshot"},
                     {"source", "cam-0"},
                     {"detections", json::array({{{"id", "human:1"}}})}}}};
  DisplayOptions brief;
  brief.brief = true;
  EXPECT_EQ(RenderReply("GetExecutionState", state, brief),
            "Skill Execution State\nexecution=exec-1 revision=4 "
            "published-at=2026-10-04T12:00:00Z\nsource=cam-0 detections=1\nid=human:1");
  EXPECT_NE(RenderReply("GetExecutionState", state, DisplayOptions{}).find("Payload"),
            std::string::npos);
}

TEST(FormatTest, NotificationIsAOneLiner) {
  const json params{{"uri", "robots/demo/scene/state"}};
  EXPECT_EQ(FormatNotification(params), "[update] robots/demo/scene/state");
}

}  // namespace
}  // namespace eos::ext::repl

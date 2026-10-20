#include <gtest/gtest.h>
#include <jpeglib.h>

#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "src/eos/runtime/init/runtime.h"
#include "src/eos/runtime/models/camera_pose.h"
#include "src/eos/runtime/models/runtime.h"

namespace eos::runtime::init {
namespace {

struct CapturedRequests {
  std::mutex mutex;
  std::map<std::string, std::vector<nlohmann::json>> by_model;
  nlohmann::json pose_response;
};

class CaptureSession final : public models::StreamSession {
 public:
  CaptureSession(std::string id, std::shared_ptr<CapturedRequests> captured)
      : id_(std::move(id)), captured_(std::move(captured)) {}

  models::CallResult Call(const nlohmann::json& request,
                          const models::CancelRequested& cancel_requested) override {
    if (cancel_requested && cancel_requested()) {
      return {models::CallResult::Status::kUnavailable, 0, {}, "cancelled"};
    }
    std::lock_guard lock(captured_->mutex);
    captured_->by_model[id_].push_back(request);
    nlohmann::json response = nlohmann::json::object();
    if (id_ == "face") {
      response = {
          {"operation", "recognize"},
          {"data",
           {{"faces", nlohmann::json::array()}, {"face_count", static_cast<std::uint64_t>(0)}}}};
    } else if (id_ == "pose") {
      response = captured_->pose_response.is_null()
                     ? nlohmann::json{{"persons", nlohmann::json::array()}, {"person_count", 0}}
                     : captured_->pose_response;
    }
    return {models::CallResult::Status::kCompleted, 200, std::move(response), {}};
  }

  void Close() noexcept override {}

 private:
  std::string id_;
  std::shared_ptr<CapturedRequests> captured_;
};

class CaptureAdapter final : public models::Adapter {
 public:
  explicit CaptureAdapter(std::shared_ptr<CapturedRequests> captured)
      : captured_(std::move(captured)) {}

  nlohmann::json PrepareRequest(const models::ModelConfig&,
                                const nlohmann::json& request) const override {
    return request;
  }
  nlohmann::json DecodeResponse(const models::ModelConfig&, std::string_view,
                                std::string_view) const override {
    return nlohmann::json::object();
  }
  void NormalizeResponse(const models::ModelConfig&, nlohmann::json*) const override {}
  std::unique_ptr<models::StreamSession> CreateStreamSession(
      const models::ModelConfig& model) const override {
    return std::make_unique<CaptureSession>(model.id, captured_);
  }

 private:
  std::shared_ptr<CapturedRequests> captured_;
};

models::ModelConfig Model(std::string id, models::Capability capability) {
  models::ModelConfig model;
  model.id = std::move(id);
  model.capability = capability;
  model.endpoint = "ws://127.0.0.1:1/infer";
  model.transport = models::Transport::kWebSocket;
  model.request_contract = "request";
  model.response_contract = "response";
  return model;
}

std::vector<std::uint8_t> TestJpeg() {
  std::vector<std::uint8_t> pixels(4 * 2 * 3, 120);
  jpeg_compress_struct encoder{};
  jpeg_error_mgr error{};
  encoder.err = jpeg_std_error(&error);
  jpeg_create_compress(&encoder);
  unsigned char* output = nullptr;
  unsigned long size = 0;
  jpeg_mem_dest(&encoder, &output, &size);
  encoder.image_width = 4;
  encoder.image_height = 2;
  encoder.input_components = 3;
  encoder.in_color_space = JCS_RGB;
  jpeg_set_defaults(&encoder);
  jpeg_start_compress(&encoder, TRUE);
  while (encoder.next_scanline < encoder.image_height) {
    JSAMPROW row = pixels.data() + encoder.next_scanline * 4 * 3;
    jpeg_write_scanlines(&encoder, &row, 1);
  }
  jpeg_finish_compress(&encoder);
  std::vector<std::uint8_t> result(output, output + size);
  jpeg_destroy_compress(&encoder);
  std::free(output);
  return result;
}

TEST(SkillInferenceRunnerTest, RotatedPoseUsesOneExactFrameAndHonorsCancellation) {
  auto captured = std::make_shared<CapturedRequests>();
  models::Runtime runtime(
      {Model("pose", models::Capability::kHumanPose)},
      [](const std::string&, const nlohmann::json&) {},
      [captured](const models::ModelConfig&) {
        return std::make_shared<CaptureAdapter>(captured);
      });
  models::CameraPoseStore store;
  models::CameraPoseStore::Snapshot snapshot;
  snapshot.device_id = "camera.front";
  snapshot.generation = 9;
  snapshot.jpeg = std::make_shared<const std::vector<std::uint8_t>>(TestJpeg());
  snapshot.image_width_px = 4;
  snapshot.image_height_px = 2;
  snapshot.depth_u16 = std::make_shared<const std::vector<std::uint16_t>>(
      std::vector<std::uint16_t>{1, 2, 3, 4, 5, 6, 7, 8});
  snapshot.depth_scale = 0.001;
  store.SetDetection(std::move(snapshot));
  const InferenceModelBindings bindings{"", "", "pose", "", ""};
  const skills::InferenceRequest request{skills::InferenceAuthority::kHumanPose,
                                         "camera.front", 1, 9,
                                         {{"rotation_degrees", {-90, 90}}}};
  const auto result = RunSkillInference(runtime, store, bindings, request);
  ASSERT_EQ(result.value.at("rotations").size(), 2u);
  EXPECT_EQ(result.value.at("rotations").at(0).at("degrees"), -90);
  EXPECT_EQ(result.value.at("rotations").at(1).at("degrees"), 90);
  ASSERT_EQ(captured->by_model.at("pose").size(), 2u);
  for (const auto& provider_request : captured->by_model.at("pose")) {
    EXPECT_FALSE(provider_request.contains("persons"));
    EXPECT_EQ(provider_request.at("input").at("media_type"), "image/jpeg");
    EXPECT_EQ(provider_request.at("depth_input").at("media_type"), "image/png");
    EXPECT_EQ(provider_request.at("depth_scale"), 0.001);
  }
  captured->by_model["pose"].clear();
  EXPECT_THROW(RunSkillInference(runtime, store, bindings, request, [captured] {
                 std::lock_guard lock(captured->mutex);
                 return captured->by_model["pose"].size() >= 1;
               }), std::runtime_error);
  EXPECT_EQ(captured->by_model.at("pose").size(), 1u);
  for (std::uint64_t generation = 10; generation < 15; ++generation) {
    models::CameraPoseStore::Snapshot newer;
    newer.device_id = "camera.front";
    newer.generation = generation;
    store.SetDetection(std::move(newer));
  }
  ASSERT_TRUE(store.Find("camera.front", 1, 9));
  EXPECT_THROW(RunSkillInference(runtime, store, bindings,
                                 {skills::InferenceAuthority::kFaceRecognition,
                                  "camera.front", 1, 9, {{"rotation_degrees", -90}}}),
               std::runtime_error);
  EXPECT_FALSE(store.Find("camera.front", 1, 9));
}

TEST(SkillInferenceRunnerTest, LockedPoseRotationMakesOneCallAndReturnsSourceGeometry) {
  auto captured = std::make_shared<CapturedRequests>();
  nlohmann::json points = nlohmann::json::array();
  nlohmann::json scores = nlohmann::json::array();
  for (int index = 0; index < 17; ++index) {
    points.push_back({0.0, 0.0});
    scores.push_back(0.9);
  }
  captured->pose_response = {{"persons", {{{"bbox", {0.0, 0.0, 2.0, 4.0}},
                                              {"keypoints", points},
                                              {"keypoint_scores", scores},
                                              {"bearing_rad", 0.25}}}}};
  models::Runtime runtime(
      {Model("pose", models::Capability::kHumanPose)},
      [](const std::string&, const nlohmann::json&) {},
      [captured](const models::ModelConfig&) {
        return std::make_shared<CaptureAdapter>(captured);
      });
  models::CameraPoseStore store;
  models::CameraPoseStore::Snapshot snapshot;
  snapshot.device_id = "camera.front";
  snapshot.generation = 9;
  snapshot.jpeg = std::make_shared<const std::vector<std::uint8_t>>(TestJpeg());
  snapshot.image_width_px = 4;
  snapshot.image_height_px = 2;
  snapshot.depth_u16 = std::make_shared<const std::vector<std::uint16_t>>(
      std::vector<std::uint16_t>{1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000});
  snapshot.depth_scale = 0.001;
  store.SetDetection(std::move(snapshot));
  const InferenceModelBindings bindings{"", "", "pose", "", ""};

  for (const int degrees : {-90, 90}) {
    const auto before = captured->by_model["pose"].size();
    const auto result = RunSkillInference(
        runtime, store, bindings,
        {skills::InferenceAuthority::kHumanPose, "camera.front", 1, 9,
         {{"rotation_degrees", degrees}}});
    ASSERT_EQ(captured->by_model.at("pose").size(), before + 1);
    ASSERT_EQ(result.value.at("rotations").size(), 1u);
    const auto& rotation = result.value.at("rotations").at(0);
    EXPECT_EQ(rotation.at("degrees"), degrees);
    const auto& person = rotation.at("persons").at(0);
    EXPECT_EQ(person.at("bbox"), nlohmann::json({0.0, 0.0, 4.0, 2.0}));
    EXPECT_EQ(person.at("rotated_bbox"), nlohmann::json({0.0, 0.0, 2.0, 4.0}));
    EXPECT_EQ(person.at("keypoints").at(0),
              degrees == -90 ? nlohmann::json({0.0, 1.0})
                             : nlohmann::json({3.0, 0.0}));
    EXPECT_EQ(person.at("keypoint_depth_m").at(0), 1.0);
    EXPECT_FALSE(person.contains("bearing_rad"));
  }
  for (const nlohmann::json invalid : {nlohmann::json(0), nlohmann::json(180),
                                       nlohmann::json::array({90, -90}),
                                       nlohmann::json::array({-90}),
                                       nlohmann::json("-90")}) {
    EXPECT_THROW(RunSkillInference(
                     runtime, store, bindings,
                     {skills::InferenceAuthority::kHumanPose, "camera.front", 1, 9,
                      {{"rotation_degrees", invalid}}}),
                 std::invalid_argument);
  }
  EXPECT_EQ(captured->by_model.at("pose").size(), 2u);
}

TEST(SkillInferenceRunnerTest, ShapesInlineVisionWithoutAnActiveCameraStream) {
  auto captured = std::make_shared<CapturedRequests>();
  models::Runtime runtime(
      {Model("human", models::Capability::kHuman), Model("pose", models::Capability::kHumanPose),
       Model("face", models::Capability::kFace)},
      [](const std::string&, const nlohmann::json&) {},
      [captured](const models::ModelConfig&) {
        return std::make_shared<CaptureAdapter>(captured);
      });
  models::CameraPoseStore empty_store;
  const InferenceModelBindings bindings{"", "human", "pose", "face", ""};
  const nlohmann::json input{
      {"data_b64", "AAAA"}, {"filename", "image.jpg"}, {"media_type", "image/jpeg"}};
  const nlohmann::json depth{
      {"data_b64", "AQID"}, {"filename", "depth.png"}, {"media_type", "image/png"}};

  const auto human = RunSkillInference(
      runtime, empty_store, bindings,
      {skills::InferenceAuthority::kHuman,
       {},
       0,
       0,
       {{"input", input}, {"depth_input", depth}, {"depth_scale", 0.001}, {"conf", 0.4}}});
  const nlohmann::json persons = {{{"track_id", 7}, {"bbox", {1, 2, 30, 40}}, {"confidence", 0.9}}};
  const auto pose = RunSkillInference(
      runtime, empty_store, bindings,
      {skills::InferenceAuthority::kHumanPose,
       {},
       0,
       0,
       {{"input", input}, {"persons", persons}, {"conf", 0.3}, {"kpt_conf", 0.5}}});
  const auto face = RunSkillInference(runtime, empty_store, bindings,
                                      {skills::InferenceAuthority::kFaceRecognition,
                                       {},
                                       0,
                                       0,
                                       {{"input", input}, {"return_image", true}}});

  EXPECT_TRUE(human.source.empty());
  EXPECT_EQ(human.source_sequence, 0u);
  EXPECT_TRUE(pose.source.empty());
  EXPECT_TRUE(face.source.empty());
  EXPECT_EQ(captured->by_model.at("human").at(0),
            nlohmann::json(
                {{"input", input}, {"depth_input", depth}, {"depth_scale", 0.001}, {"conf", 0.4}}));
  EXPECT_EQ(
      captured->by_model.at("pose").at(0),
      nlohmann::json({{"input", input}, {"persons", persons}, {"conf", 0.3}, {"kpt_conf", 0.5}}));
  EXPECT_EQ(captured->by_model.at("face").at(0),
            nlohmann::json({{"operation", "recognize"}, {"input", input}, {"return_image", true}}));
}

TEST(SkillInferenceRunnerTest, RejectsMixedModeAndPreservesExactRetainedFrame) {
  auto captured = std::make_shared<CapturedRequests>();
  models::Runtime runtime(
      {Model("human", models::Capability::kHuman)},
      [](const std::string&, const nlohmann::json&) {},
      [captured](const models::ModelConfig&) {
        return std::make_shared<CaptureAdapter>(captured);
      });
  models::CameraPoseStore store;
  models::CameraPoseStore::Snapshot snapshot;
  snapshot.device_id = "camera.front";
  snapshot.generation = 9;
  snapshot.jpeg =
      std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>{1, 2, 3});
  snapshot.image_width_px = 2;
  snapshot.image_height_px = 1;
  snapshot.depth_u16 =
      std::make_shared<const std::vector<std::uint16_t>>(std::vector<std::uint16_t>{1000, 2000});
  snapshot.depth_scale = 0.001;
  store.SetDetection(std::move(snapshot));
  const InferenceModelBindings bindings{"", "human", "", "", ""};

  EXPECT_THROW(RunSkillInference(runtime, store, bindings,
                                 {skills::InferenceAuthority::kHuman,
                                  "camera.front",
                                  1,
                                  9,
                                  {{"input", {{"data_b64", "AAAA"}}}}}),
               std::invalid_argument);
  const auto retained = RunSkillInference(
      runtime, store, bindings,
      {skills::InferenceAuthority::kHuman, "camera.front", 1, 9, {{"conf", 0.6}}});

  EXPECT_EQ(retained.source, "camera.front");
  EXPECT_EQ(retained.source_sequence, 1u);
  EXPECT_EQ(retained.source_generation, 9u);
  EXPECT_EQ(captured->by_model.at("human").size(), 1u);
  EXPECT_EQ(captured->by_model.at("human").at(0).at("input").at("data_b64"), "AQID");
  EXPECT_EQ(captured->by_model.at("human").at(0).at("conf"), 0.6);
  EXPECT_EQ(captured->by_model.at("human").at(0).at("depth_input").at("media_type"), "image/png");
  EXPECT_EQ(captured->by_model.at("human").at(0).at("depth_scale"), 0.001);
}

TEST(SkillInferenceRunnerTest, RoutesChatOptionsThroughInjectedBindingWithoutProvenance) {
  auto captured = std::make_shared<CapturedRequests>();
  models::Runtime runtime(
      {Model("injected-chat", models::Capability::kChatCompletion)},
      [](const std::string&, const nlohmann::json&) {},
      [captured](const models::ModelConfig&) {
        return std::make_shared<CaptureAdapter>(captured);
      });
  models::CameraPoseStore empty_store;
  const InferenceModelBindings bindings{"", "", "", "", "injected-chat"};
  const nlohmann::json options{
      {"messages", {{{"role", "user"}, {"content", "你好"}}}},
      {"temperature", 0.3}, {"max_tokens", 128}, {"stream", false}};

  const auto result = RunSkillInference(
      runtime, empty_store, bindings,
      {skills::InferenceAuthority::kChatCompletion, {}, 0, 0, options});

  EXPECT_TRUE(result.source.empty());
  EXPECT_EQ(result.source_sequence, 0u);
  ASSERT_EQ(captured->by_model.at("injected-chat").size(), 1u);
  EXPECT_EQ(captured->by_model.at("injected-chat").front(), options);
  EXPECT_THROW(RunSkillInference(
                   runtime, empty_store, bindings,
                   {skills::InferenceAuthority::kChatCompletion, "camera.front", 1, 1, options}),
               std::invalid_argument);
}

TEST(SkillInferenceRunnerTest, VlmAcceptsTextOrOneCameraImage) {
  auto captured = std::make_shared<CapturedRequests>();
  models::Runtime runtime(
      {Model("vla-gemma4", models::Capability::kVlm)},
      [](const std::string&, const nlohmann::json&) {},
      [captured](const models::ModelConfig&) {
        return std::make_shared<CaptureAdapter>(captured);
      });
  models::CameraPoseStore store;
  const InferenceModelBindings bindings{"", "", "", "", "", "vla-gemma4"};
  const nlohmann::json messages{{"messages", {{{"role", "user"}, {"content", "你好"}}}}};
  const auto result = RunSkillInference(runtime, store, bindings,
      {skills::InferenceAuthority::kVlm, {}, 0, 0, messages});
  EXPECT_TRUE(result.source.empty());
  ASSERT_EQ(captured->by_model.at("vla-gemma4").size(), 1u);
  EXPECT_EQ(captured->by_model.at("vla-gemma4").front(), messages);
  EXPECT_THROW(RunSkillInference(runtime, store, bindings,
      {skills::InferenceAuthority::kVlm, "camera.front", 1, 1, messages}),
      std::invalid_argument);
  EXPECT_THROW(RunSkillInference(runtime, store, bindings,
      {skills::InferenceAuthority::kVlm, "camera.front", 1, 0, messages}),
      std::invalid_argument);
  EXPECT_THROW(RunSkillInference(runtime, store, bindings,
      {skills::InferenceAuthority::kVlm, {}, 0, 0,
       { {"messages", messages.at("messages")}, {"depth_input", nlohmann::json::object()} }}),
      std::invalid_argument);
}

TEST(SkillInferenceRunnerTest, InjectsOnlyEnabledExactChatBinding) {
  auto captured = std::make_shared<CapturedRequests>();
  const auto adapter = [captured](const models::ModelConfig&) {
    return std::make_shared<CaptureAdapter>(captured);
  };
  models::Runtime enabled(
      {Model("gemma4-26b-chat", models::Capability::kChatCompletion)},
      [](const std::string&, const nlohmann::json&) {}, adapter);
  auto disabled_config = Model("gemma4-26b-chat", models::Capability::kChatCompletion);
  disabled_config.enabled = false;
  models::Runtime disabled({disabled_config}, [](const std::string&, const nlohmann::json&) {},
                           adapter);
  models::Runtime wrong({Model("gemma4-26b-chat", models::Capability::kHuman)},
                        [](const std::string&, const nlohmann::json&) {}, adapter);
  models::Runtime missing({}, [](const std::string&, const nlohmann::json&) {}, adapter);

  EXPECT_EQ(ResolveSkillChatModel(enabled), "gemma4-26b-chat");
  EXPECT_TRUE(ResolveSkillChatModel(disabled).empty());
  EXPECT_TRUE(ResolveSkillChatModel(wrong).empty());
  EXPECT_TRUE(ResolveSkillChatModel(missing).empty());
}

}  // namespace
}  // namespace eos::runtime::init

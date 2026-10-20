#include "src/eos/ext/models/external_adapter.h"

#include <gtest/gtest.h>

#include <stdexcept>

namespace eos::ext::models {
namespace {

using runtime::models::Capability;
using runtime::models::ModelConfig;
using runtime::models::WireFormat;

ModelConfig Config(std::string adapter, Capability capability, WireFormat wire_format) {
  ModelConfig model;
  model.id = "test-model";
  model.adapter = std::move(adapter);
  model.capability = capability;
  model.wire_format = wire_format;
  return model;
}

TEST(ExternalModelAdapterTest, CanonicalLeavesPayloadUnchanged) {
  const ModelConfig model = Config("canonical", Capability::kVad, WireFormat::kJson);
  const auto adapter = CreateAdapter(model);
  const nlohmann::json request{{"input", {{"data_b64", "AAAA"}}}, {"stream", false}};

  EXPECT_EQ(adapter->PrepareRequest(model, request), request);

  nlohmann::json response{{"speech", false}};
  adapter->NormalizeResponse(model, &response);
  EXPECT_EQ(response, nlohmann::json({{"speech", false}}));
}

TEST(ExternalModelAdapterTest, OpenAiChatMapsOnlyCanonicalTextFields) {
  ModelConfig model = Config("openai-chat-v1", Capability::kChatCompletion, WireFormat::kJson);
  model.provider_model_id = "Gemma4-26B";
  const auto adapter = CreateAdapter(model);
  const nlohmann::json messages = {{{"role", "system"}, {"content", "Be concise."}},
                                   {{"role", "user"}, {"content", "Hello"}}};

  EXPECT_EQ(adapter->PrepareRequest(
                model, {{"messages", messages},
                        {"temperature", 0.3},
                        {"max_tokens", 128},
                        {"stream", true},
                        {"tools", nlohmann::json::array()},
                        {"reasoning", "hidden"}}),
            (nlohmann::json{{"model", "Gemma4-26B"},
                             {"messages", messages},
                             {"chat_template_kwargs", {{"enable_thinking", false}}},
                             {"temperature", 0.3},
                            {"max_tokens", 128},
                            {"stream", false}}));

  nlohmann::json response = {
      {"choices", {{{"message", {{"content", "Hi"}, {"reasoning", "private"},
                                    {"tool_calls", nlohmann::json::array()}}}}}},
      {"usage", {{"total_tokens", 5}}}};
  adapter->NormalizeResponse(model, &response);
  EXPECT_EQ(response, (nlohmann::json{{"text", "Hi"}}));
}

TEST(ExternalModelAdapterTest, OpenAiChatRejectsMalformedOrBlankText) {
  ModelConfig model = Config("openai-chat-v1", Capability::kChatCompletion, WireFormat::kJson);
  model.provider_model_id = "Gemma4-26B";
  const auto adapter = CreateAdapter(model);
  std::vector<nlohmann::json> malformed = {
      {{"choices", nlohmann::json::object()}},
      {{"choices", nlohmann::json::array()}},
      {{"choices", {"bad entry"}}},
      {{"choices", {{{"message", "bad message"}}}}},
      {{"choices", {{{"message", nlohmann::json::object()}}}}},
      {{"choices", {{{"message", {{"content", nullptr}}}}}}},
      {{"choices", {{{"message", {{"content", 7}}}}}}},
      {{"choices", {{{"message", {{"content", ""}}}}}}},
      {{"choices", {{{"message", {{"content", " \t\r\n"}}}}}}},
  };
  for (auto& response : malformed) {
    EXPECT_THROW(adapter->NormalizeResponse(model, &response), std::runtime_error);
  }
}

TEST(ExternalModelAdapterTest, OpenAiVlmSupportsTextAndOptionalImage) {
  ModelConfig model = Config("openai-vlm-v1", Capability::kVlm, WireFormat::kJson);
  model.provider_model_id = "Gemma4-26B";
  const auto adapter = CreateAdapter(model);
  const nlohmann::json messages = {{{"role", "system"}, {"content", "Be concise."}},
                                   {{"role", "user"}, {"content", "What is here?"}}};
  const auto text_payload = adapter->PrepareRequest(model, {{"messages", messages}});
  EXPECT_EQ(text_payload.at("messages"), messages);
  EXPECT_EQ(text_payload.at("stream"), false);
  EXPECT_EQ(text_payload.at("chat_template_kwargs").at("enable_thinking"), false);
  EXPECT_EQ(adapter->PrepareRequest(model, {{"messages", messages}, {"reasoning", true}})
                .at("chat_template_kwargs").at("enable_thinking"), true);

  const auto image_payload = adapter->PrepareRequest(
      model, {{"messages", messages},
              {"input", {{"data_b64", "AAAA"}, {"media_type", "image/jpeg"}}}});
  EXPECT_EQ(image_payload.at("messages").at(0), messages.at(0));
  EXPECT_EQ(image_payload.at("messages").at(1).at("content").at(0).at("text"), "What is here?");
  EXPECT_EQ(image_payload.at("messages").at(1).at("content").at(1).at("image_url").at("url"),
            "data:image/jpeg;base64,AAAA");
  EXPECT_THROW(adapter->PrepareRequest(model,
      {{"messages", {{{"role", "system"}, {"content", "No user."}}}},
       {"input", {{"data_b64", "AAAA"}, {"media_type", "image/jpeg"}}}}),
      std::invalid_argument);

  auto answer = adapter->DecodeResponse(model, "application/json",
      R"({"choices":[{"message":{"content":"A chair"}}]})");
  adapter->NormalizeResponse(model, &answer);
  EXPECT_EQ(answer, (nlohmann::json{{"text", "A chair"}}));
  answer = {{"choices", {{{"message", {{"content", "  "}}}}}}};
  EXPECT_THROW(adapter->NormalizeResponse(model, &answer), std::runtime_error);
}

TEST(ExternalModelAdapterTest, UnifiedGemmaRoutesPostureAndGeneralTextSeparately) {
  ModelConfig model = Config("gemma4-unified-v1", Capability::kVlm, WireFormat::kJson);
  model.provider_model_id = "Gemma4-26B";
  const auto adapter = CreateAdapter(model);
  const nlohmann::json posture{{"input", {{"data_b64", "AAAA"}, {"media_type", "image/jpeg"}}},
                               {"bbox", {10, 20, 100, 200}}};
  EXPECT_EQ(adapter->PrepareRequest(model, posture).at("response_format").at("type"),
            "json_object");
  nlohmann::json response = nlohmann::json::parse(
      R"({"choices":[{"message":{"content":"{\"posture_state\":\"standing\",\"action\":\"none\"}"}}]})");
  adapter->NormalizeResponseForRequest(model, posture, &response);
  EXPECT_EQ(response.at("posture_state"), "standing");
  response = nlohmann::json::parse(
      R"({"choices":[{"message":{"content":"{\"posture_state\":\"standing\",\"action\":\"none\"}"}}]})");
  adapter->NormalizeResponseForRequest(model,
      {{"messages", {{{"role", "user"}, {"content", "Return JSON"}}}}}, &response);
  EXPECT_TRUE(response.contains("text"));
  nlohmann::json health = {{"status", "ok"}};
  adapter->NormalizeHealthResponse(model, &health);
  EXPECT_EQ(health.at("status"), "ready");
  EXPECT_EQ(health.at("model").at("id"), "Gemma4-26B");
}

TEST(ExternalModelAdapterTest, Gemma4ClassifiesOneTargetAndRejectsInvalidAnswer) {
  ModelConfig model = Config("gemma4-posture-v1", Capability::kPostureClassification,
                             WireFormat::kJson);
  model.provider_model_id = "gemma-4";
  const auto adapter = CreateAdapter(model);
  const auto payload = adapter->PrepareRequest(
      model, {{"input", {{"data_b64", "AAAA"}, {"media_type", "image/jpeg"}}},
              {"bbox", {10, 20, 100, 200}}});
  EXPECT_EQ(payload.at("model"), "gemma-4");
  EXPECT_EQ(payload.at("chat_template_kwargs").at("enable_thinking"), false);
  EXPECT_EQ(payload.at("response_format").at("type"), "json_object");
  EXPECT_EQ(payload.at("messages").at(0).at("content").at(1).at("image_url").at("url"),
            "data:image/jpeg;base64,AAAA");
  EXPECT_THROW(adapter->PrepareRequest(model, {{"input", {{"data_b64", "AAAA"}}},
                                               {"bbox", {100, 20, 10, 200}}}),
               std::invalid_argument);

  auto answer = adapter->DecodeResponse(
      model, "application/json",
      R"({"choices":[{"message":{"content":"{\"posture_state\":\"sitting\",\"action\":\"waving\"}"}}]})");
  EXPECT_NO_THROW(adapter->NormalizeResponse(model, &answer));
  EXPECT_EQ(answer.at("posture_state"), "sitting");
  EXPECT_EQ(answer.at("action"), "waving");
  answer = {{"posture_state", "standing"}, {"action", "fallen"}};
  EXPECT_THROW(adapter->NormalizeResponse(model, &answer), std::invalid_argument);
  answer = {{"posture_state", "jumping"}, {"action", "none"}};
  EXPECT_THROW(adapter->NormalizeResponse(model, &answer), std::invalid_argument);
  EXPECT_THROW(adapter->DecodeResponse(model, "application/json", "{}"), std::exception);
}

TEST(ExternalModelAdapterTest, Audio8MapsCanonicalFields) {
  ModelConfig model = Config("audio8-speech-v1", Capability::kTts, WireFormat::kJson);
  model.provider_model_id = "audio8/tts-0.6b";
  const auto adapter = CreateAdapter(model);

  const nlohmann::json payload =
      adapter->PrepareRequest(model, {{"text", "hello"},
                                      {"stream", false},
                                      {"temperature", 0.7},
                                      {"top_p", 0.9},
                                      {"reference_audio_b64", "AAAA"},
                                      {"reference_audio_media_type", "audio/wav"},
                                      {"prompt_text", "reference words"}});

  EXPECT_EQ(payload, (nlohmann::json{{"input", "hello"},
                                     {"model", "audio8/tts-0.6b"},
                                     {"response_format", "wav"},
                                     {"stream", false},
                                     {"temperature", 0.7},
                                     {"top_p", 0.9},
                                     {"ref_audio", "AAAA"},
                                     {"ref_audio_media_type", "audio/wav"},
                                     {"ref_text", "reference words"}}));
  EXPECT_EQ(
      adapter->DecodeResponse(model, "audio/wav; charset=binary", "RIFF"),
      (nlohmann::json{{"data_b64", "UklGRg=="}, {"media_type", "audio/wav"}, {"format", "wav"}}));
  EXPECT_THROW(adapter->DecodeResponse(model, "application/json", "{}"), std::runtime_error);

  nlohmann::json health{{"status", "healthy"},
                        {"running", true},
                        {"stages", {"preprocessing", "tts_engine", "vocoder"}}};
  adapter->NormalizeHealthResponse(model, &health);
  EXPECT_EQ(health.at("status"), "ready");
  EXPECT_EQ(health.at("model").at("id"), "audio8/tts-0.6b");
  EXPECT_FALSE(health.contains("message"));

  nlohmann::json stopped{{"status", "healthy"}, {"running", false}};
  adapter->NormalizeHealthResponse(model, &stopped);
  EXPECT_EQ(stopped.at("status"), "unavailable");
  EXPECT_EQ(stopped.at("message"), "provider status is 'healthy'");
}

TEST(ExternalModelAdapterTest, Qwen3AsrMapsCanonicalRequestToProviderJsonFields) {
  ModelConfig model = Config("qwen3-asr-v1", Capability::kAsr, WireFormat::kJson);
  model.provider_model_id = "qwen3-asr-0.6b";
  const auto adapter = CreateAdapter(model);
  const nlohmann::json payload = adapter->PrepareRequest(
      model, {{"input", {{"data_b64", "AAAA"}}},
              {"language", "en"},
              {"prompt", "product names"},
              {"temperature", 0.2},
              {"stream", false}});

  EXPECT_EQ(payload, (nlohmann::json{{"input", {{"data_b64", "AAAA"}}},
                                     {"language", "en"},
                                     {"prompt", "product names"},
                                     {"temperature", 0.2},
                                     {"response_format", "json"}}));

  nlohmann::json health{{"status", "ok"}, {"model", "qwen3-asr-0.6b"}};
  adapter->NormalizeHealthResponse(model, &health);
  EXPECT_EQ(health.at("status"), "ready");
  EXPECT_EQ(health.at("model").at("id"), "qwen3-asr-0.6b");
  EXPECT_EQ(health.at("capabilities"), nlohmann::json({"audio.transcription"}));
}

TEST(ExternalModelAdapterTest, SensevoiceVadStripsUnsupportedFieldsAndNormalizesHealth) {
  ModelConfig model = Config("sensevoice-vad-v1", Capability::kVad, WireFormat::kMultipart);
  model.provider_model_id = "silero-vad";
  const auto adapter = CreateAdapter(model);
  EXPECT_EQ(adapter->PrepareRequest(
                model, {{"input", {{"data_b64", "AAAA"}}},
                        {"threshold", 0.5}, {"stream", false}}),
            (nlohmann::json{{"input",
                             {{"data_b64", "AAAA"},
                              {"filename", "audio.wav"},
                              {"media_type", "audio/wav"},
                              {"sample_rate_hz", 16000},
                              {"channels", 1}}},
                             {"threshold", 0.5}}));
  nlohmann::json health{
      {"status", "ok"}, {"model", "sensevoice-vad"}, {"asr_backend", "disabled"}};
  adapter->NormalizeHealthResponse(model, &health);
  EXPECT_EQ(health.at("status"), "ready");
  EXPECT_EQ(health.at("model").at("id"), "silero-vad");
  EXPECT_FALSE(health.contains("asr_backend"));
}

TEST(ExternalModelAdapterTest, ArcFaceStreamPassesCanonicalOperationsThrough) {
  ModelConfig model = Config("arcface-stream-v1", Capability::kFace, WireFormat::kJson);
  const auto adapter = CreateAdapter(model);

  const nlohmann::json recognize = adapter->PrepareRequest(
      model, {{"operation", "recognize"}, {"input", {{"data_b64", "AAAA"}}}});
  EXPECT_EQ(recognize.at("operation"), "recognize");
  EXPECT_TRUE(recognize.contains("input"));
  nlohmann::json response = {{"status", "success"},
                             {"operation", "recognize"},
                             {"message", "ok"},
                             {"data",
                              {{"face_count", 2},
                               {"faces",
                                {{{"identity", "Alice"},
                                  {"known", true},
                                  {"similarity", 0.8},
                                  {"bbox", {1, 2, 3, 4}},
                                  {"direction_px", -2.0}},
                                 {{"identity", "Unknown"},
                                  {"known", false},
                                  {"similarity", 0.0},
                                  {"bbox", {5, 6, 7, 8}}}}}}}};
  const nlohmann::json expected_response = response;
  adapter->NormalizeResponse(model, &response);
  EXPECT_EQ(response, expected_response);

  nlohmann::json registered = {{"status", "success"},
                               {"operation", "register"},
                               {"message", "ok"},
                               {"data", {{"name", "Alice"}}}};
  const nlohmann::json expected_registered = registered;
  adapter->NormalizeResponse(model, &registered);
  EXPECT_EQ(registered, expected_registered);

  nlohmann::json health = {{"status", "ok"},
                           {"model", "arcface"},
                           {"model_name", "buffalo_l"},
                           {"providers", {"CPUExecutionProvider"}},
                           {"db_exists", true}};
  adapter->NormalizeHealthResponse(model, &health);
  EXPECT_EQ(health.at("status"), "ready");
  EXPECT_EQ(health.at("model").at("id"), "arcface");
  EXPECT_EQ(health.at("model").at("version"), "buffalo_l");
  EXPECT_EQ(health.at("capabilities"), nlohmann::json({"vision.face"}));

  nlohmann::json no_database = {
      {"status", "ok"}, {"model", "arcface"}, {"model_name", "buffalo_l"}, {"db_exists", false}};
  adapter->NormalizeHealthResponse(model, &no_database);
  EXPECT_EQ(no_database.at("status"), "ready");
}

TEST(ExternalModelAdapterTest, LegacyVisionNormalizesDetection) {
  const ModelConfig model = Config("legacy-vision", Capability::kDetection, WireFormat::kMultipart);
  const auto adapter = CreateAdapter(model);
  nlohmann::json response{
      {"persons", {{{"bbox", {1, 2, 3, 4}}, {"confidence", 0.8}, {"provider", "private"}}}}};

  adapter->NormalizeResponse(model, &response);

  EXPECT_EQ(
      response,
      (nlohmann::json{
          {"objects",
           {{{"label", "person"}, {"class_id", 0}, {"bbox", {1, 2, 3, 4}}, {"confidence", 0.8}}}},
          {"object_count", 1}}));
}

TEST(ExternalModelAdapterTest, LegacyVisionFiltersAndClampsPose) {
  const ModelConfig model = Config("legacy-vision", Capability::kPose, WireFormat::kJson);
  const auto adapter = CreateAdapter(model);
  nlohmann::json response{{"persons",
                           {{{"track_id", 4},
                             {"bbox", {1, 2, 3, 4}},
                             {"keypoint_scores", {-0.2, 0.5, 1.2}},
                             {"provider", "private"}}}}};

  adapter->NormalizeResponse(model, &response);

  ASSERT_EQ(response.at("person_count"), 1);
  const auto& person = response.at("persons").at(0);
  EXPECT_EQ(person.at("keypoint_scores"), nlohmann::json({0.0, 0.5, 1.0}));
  EXPECT_EQ(person.at("distance_m"), nullptr);
  EXPECT_FALSE(person.contains("provider"));
}

TEST(ExternalModelAdapterTest, RtmposeStreamMapsFrameAndNormalizesResponse) {
  const ModelConfig model = Config("rtmpose-stream-v1", Capability::kPose, WireFormat::kJson);
  const auto adapter = CreateAdapter(model);

  const nlohmann::json payload = adapter->PrepareRequest(
      model, {{"input", {{"data_b64", "rgb"}, {"filename", "camera.jpg"},
                           {"media_type", "image/jpeg"}}},
              {"depth_input", {{"data_b64", "depth"}, {"filename", "depth.png"},
                                 {"media_type", "image/png"}}},
               {"depth_scale", 0.001},
               {"depth_intrinsics",
                {{"width", 8},
                 {"height", 8},
                 {"fx", 6.0},
                 {"fy", 6.0},
                 {"ppx", 4.0},
                 {"ppy", 4.0},
                 {"distortion_model", "inverse_brown_conrady"},
                 {"coeffs", {0.0, 0.0, 0.0, 0.0, 0.0}}}},
               {"persons", {{{"track_id", 8}, {"bbox", {1, 2, 3, 4}}, {"confidence", 0.9}}}},
               {"conf", 0.4},
              {"kpt_conf", 0.2}});

  EXPECT_EQ(payload, (nlohmann::json{{"type", "frame"},
                                     {"image_b64", "rgb"},
                                     {"depth_b64", "depth"},
                                      {"depth_scale", 0.001},
                                      {"depth_intrinsics",
                                       {{"width", 8},
                                        {"height", 8},
                                        {"fx", 6.0},
                                        {"fy", 6.0},
                                        {"ppx", 4.0},
                                        {"ppy", 4.0},
                                        {"distortion_model", "inverse_brown_conrady"},
                                        {"coeffs", {0.0, 0.0, 0.0, 0.0, 0.0}}}},
                                      {"persons", {{{"track_id", 8},
                                                     {"bbox", {1, 2, 3, 4}},
                                                     {"confidence", 0.9}}}},
                                     {"conf", 0.4},
                                     {"kpt_conf", 0.2}}));
  nlohmann::json response{{"type", "result"},
                          {"frame_index", 7},
                          {"persons", {{{"track_id", 4},
                                        {"keypoint_scores", {-0.2, 0.5, 1.2}},
                                        {"provider", "private"}}}},
                          {"person_count", 1},
                          {"events", nlohmann::json::array()}};
  adapter->NormalizeResponse(model, &response);
  EXPECT_EQ(response.at("person_count"), 1);
  EXPECT_EQ(response.at("persons").at(0).at("keypoint_scores"),
            nlohmann::json({0.0, 0.5, 1.0}));
  EXPECT_EQ(response.at("persons").at(0).at("distance_m"), nullptr);
  EXPECT_FALSE(response.contains("type"));
  EXPECT_FALSE(response.contains("frame_index"));
  EXPECT_FALSE(response.contains("events"));
}

TEST(ExternalModelAdapterTest, RtmposeNormalizesDetectionToCanonicalObjects) {
  const ModelConfig model = Config("rtmpose-stream-v1", Capability::kHuman, WireFormat::kJson);
  const auto adapter = CreateAdapter(model);
  nlohmann::json response{
      {"type", "result"},
       {"frame_index", 9},
       {"persons", {{{"bbox", {10, 20, 30, 40}}, {"confidence", 0.75}, {"track_id", 7}}}},
       {"person_count", 1},
       {"furniture", {{{"bbox", {2, 3, 20, 30}}, {"class_name", "chair"}}}}};

  adapter->NormalizeResponse(model, &response);

  EXPECT_EQ(response,
            (nlohmann::json{{"persons",
                             {{{"bbox", {10, 20, 30, 40}},
                                {"confidence", 0.75},
                                {"track_id", 7}}}},
                             {"person_count", 1},
                             {"furniture",
                              {{{"bbox", {2, 3, 20, 30}}, {"class_name", "chair"}}}}}));
}

TEST(ExternalModelAdapterTest, RtmposeHumanPoseKeepsOnlyCanonicalPoseAndAction) {
  const ModelConfig model = Config("rtmpose-stream-v1", Capability::kHumanPose,
                                   WireFormat::kJson);
  const auto adapter = CreateAdapter(model);
  nlohmann::json keypoints = nlohmann::json::array();
  nlohmann::json scores = nlohmann::json::array();
  for (int index = 0; index < 17; ++index) {
    keypoints.push_back({index, index + 1});
    scores.push_back(0.8);
  }
  nlohmann::json response{
      {"persons",
       {{{"track_id", 7},
         {"bbox", {1, 2, 30, 40}},
         {"confidence", 0.9},
         {"distance_m", 2.5},
         {"keypoints", keypoints},
         {"keypoint_scores", scores},
         {"posture_state", "standing"},
         {"posture", "standing"},
         {"posture_confidence", 0.9},
         {"behavior_event", "none"},
         {"behavior_confidence", 0.0},
         {"pose", "stand"},
         {"pose_confidence", 0.9},
         {"action", "none"},
         {"action_confidence", 0.0}},
        {{"track_id", 8},
         {"bbox", {2, 3, 20, 30}},
         {"confidence", 0.7},
         {"keypoints", nullptr},
         {"keypoint_scores", nullptr}}}},
      {"person_count", 2}};

  adapter->NormalizeResponse(model, &response);

  ASSERT_EQ(response.at("person_count"), 1);
  EXPECT_EQ(response.at("persons").at(0).at("track_id"), 7);
  EXPECT_EQ(response.at("persons").at(0).at("keypoints").size(), 17);
  EXPECT_FALSE(response.at("persons").at(0).contains("posture_state"));
  EXPECT_FALSE(response.at("persons").at(0).contains("posture"));
  EXPECT_FALSE(response.at("persons").at(0).contains("behavior_event"));
  EXPECT_EQ(response.at("persons").at(0).at("pose"), "stand");
  EXPECT_EQ(response.at("persons").at(0).at("pose_confidence"), 0.9);
  EXPECT_EQ(response.at("persons").at(0).at("action"), "none");
  EXPECT_EQ(response.at("persons").at(0).at("action_confidence"), 0.0);
  EXPECT_EQ(response.at("persons").at(0).at("distance_m"), 2.5);
}

TEST(ExternalModelAdapterTest, RtmposeStreamMapsRgbOnlyFrame) {
  const ModelConfig model = Config("rtmpose-stream-v1", Capability::kPosture, WireFormat::kJson);
  const auto adapter = CreateAdapter(model);

  EXPECT_EQ(adapter->PrepareRequest(model, {{"input", {{"data_b64", "rgb"}}}}),
             (nlohmann::json{{"type", "frame"}, {"image_b64", "rgb"}}));
  nlohmann::json health{{"status", "ok"},
                        {"model", "yolo11n-rtmpose"},
                        {"capabilities", {"vision.object_detection", "vision.posture"}}};
  adapter->NormalizeHealthResponse(model, &health);
  EXPECT_EQ(health.at("status"), "ready");
  EXPECT_EQ(health.at("capabilities"), nlohmann::json({"vision.posture"}));
}

TEST(ExternalModelAdapterTest, Yolo11LitterMapsFrameAndPreservesObjectLabels) {
  ModelConfig model =
      Config("yolo11-litter-stream-v1", Capability::kDetection, WireFormat::kJson);
  model.provider_model_id = "yolo11-litter";
  const auto adapter = CreateAdapter(model);

  EXPECT_EQ(adapter->PrepareRequest(
                model, {{"input", {{"data_b64", "rgb"}}}, {"conf", 0.3}, {"iou", 0.4}}),
            (nlohmann::json{{"type", "frame"},
                            {"image_b64", "rgb"},
                            {"conf", 0.3},
                            {"iou", 0.4}}));

  nlohmann::json response{
      {"type", "result"},
      {"objects",
        {{{"label", "peel"},
           {"class_id", 0},
           {"source_label", "apple"},
           {"source_class_id", 0},
          {"confidence", 0.8},
          {"bbox", {1, 2, 3, 4}}},
        {{"label", "empty_box"},
         {"class_id", 1},
         {"source_label", "shupian"},
         {"source_class_id", 7},
         {"confidence", 0.7},
         {"bbox", {5, 6, 7, 8}}},
        {{"label", "bottle"},
         {"class_id", 2},
         {"source_label", "cola"},
         {"source_class_id", 4},
         {"confidence", 0.6},
         {"bbox", {9, 10, 11, 12}}},
        {{"label", "paper_ball"},
         {"class_id", 3},
         {"source_label", "paper"},
         {"source_class_id", 5},
         {"confidence", 0.5},
         {"bbox", {13, 14, 15, 16}}}}},
      {"object_count", 4}};
  adapter->NormalizeResponse(model, &response);
  EXPECT_EQ(response.at("object_count"), 4);
  EXPECT_EQ(response.at("objects").at(0).at("label"), "peel");
  EXPECT_EQ(response.at("objects").at(1).at("label"), "empty_box");
  EXPECT_EQ(response.at("objects").at(2).at("label"), "bottle");
  EXPECT_EQ(response.at("objects").at(3).at("label"), "paper_ball");
  EXPECT_EQ(response.at("objects").at(0).at("source_label"), "apple");
  EXPECT_EQ(response.at("objects").at(1).at("source_label"), "shupian");
  EXPECT_EQ(response.at("objects").at(2).at("source_label"), "cola");
  EXPECT_EQ(response.at("objects").at(3).at("source_label"), "paper");
  EXPECT_FALSE(response.contains("type"));

  nlohmann::json health{{"status", "ok"},
                         {"model", "yolo11-litter"},
                         {"classes", {"peel", "empty_box", "bottle", "paper_ball"}}};
  adapter->NormalizeHealthResponse(model, &health);
  EXPECT_EQ(health.at("status"), "ready");
  EXPECT_EQ(health.at("model").at("id"), "yolo11-litter");
  EXPECT_EQ(health.at("capabilities"), nlohmann::json({"vision.object_detection"}));
}

TEST(ExternalModelAdapterTest, Yolo11LitterRejectsMalformedResults) {
  const ModelConfig model =
      Config("yolo11-litter-stream-v1", Capability::kDetection, WireFormat::kJson);
  const auto adapter = CreateAdapter(model);
  nlohmann::json persons{{"type", "result"}, {"persons", nlohmann::json::array()}};
  EXPECT_THROW(adapter->NormalizeResponse(model, &persons), std::runtime_error);
  nlohmann::json error{{"type", "error"}, {"detail", "bad image"}};
  EXPECT_THROW(adapter->NormalizeResponse(model, &error), std::runtime_error);
}

TEST(ExternalModelAdapterTest, RejectsUnknownAndIncompatibleAdapters) {
  EXPECT_THROW(CreateAdapter(Config("unknown", Capability::kVad, WireFormat::kJson)),
               std::invalid_argument);
  EXPECT_THROW(CreateAdapter(Config("audio8-speech-v1", Capability::kAsr, WireFormat::kJson)),
               std::invalid_argument);
  EXPECT_THROW(CreateAdapter(Config("qwen3-asr-v1", Capability::kAsr, WireFormat::kMultipart)),
               std::invalid_argument);
  EXPECT_THROW(
      CreateAdapter(Config("arcface-stream-v1", Capability::kFace, WireFormat::kMultipart)),
      std::invalid_argument);
  EXPECT_THROW(CreateAdapter(Config("legacy-vision", Capability::kTts, WireFormat::kJson)),
                std::invalid_argument);
  EXPECT_THROW(CreateAdapter(Config("openai-chat-v1", Capability::kTts, WireFormat::kJson)),
                std::invalid_argument);
  EXPECT_THROW(CreateAdapter(Config("rtmpose-stream-v1", Capability::kTts, WireFormat::kJson)),
               std::invalid_argument);
  EXPECT_THROW(
      CreateAdapter(Config("rtmpose-stream-v1", Capability::kPose, WireFormat::kMultipart)),
      std::invalid_argument);
  EXPECT_THROW(
      CreateAdapter(Config("yolo11-litter-stream-v1", Capability::kPose, WireFormat::kJson)),
      std::invalid_argument);
  EXPECT_THROW(CreateAdapter(
                   Config("yolo11-litter-stream-v1", Capability::kDetection,
                          WireFormat::kMultipart)),
               std::invalid_argument);
}

}  // namespace
}  // namespace eos::ext::models

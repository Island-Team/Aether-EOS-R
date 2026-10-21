#include "src/eos/ext/backends/builtin.h"
#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

#include "src/eos/runtime/hardware/registry.h"
#include "src/eos/runtime/init/runtime.h"
#include "src/eos/runtime/models/runtime.h"

namespace eos::runtime::init {
namespace {

class TestStream final : public models::StreamSession {
 public:
  models::CallResult Call(const nlohmann::json&, const models::CancelRequested&) override {
    return {models::CallResult::Status::kCompleted, 0, nlohmann::json::object(), {}};
  }
  void Close() noexcept override {}
};

class TestAdapter final : public models::Adapter {
 public:
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
      const models::ModelConfig&) const override {
    return std::make_unique<TestStream>();
  }
};

models::Runtime ModelRuntime(std::vector<models::ModelConfig> config) {
  return models::Runtime(std::move(config), {},
                         [](const models::ModelConfig&) {
                           return std::make_shared<TestAdapter>();
                         });
}

models::ModelConfig TtsModel(bool enabled = true,
                             models::Capability capability = models::Capability::kTts) {
  models::ModelConfig model;
  model.id = "audio8";
  model.enabled = enabled;
  model.capability = capability;
  model.endpoint = "http://127.0.0.1:19003/v1/audio/speech";
  return model;
}

models::ModelConfig FaceModel() {
  models::ModelConfig model;
  model.id = "arcface-register";
  model.enabled = true;
  model.capability = models::Capability::kFace;
  model.transport = models::Transport::kWebSocket;
  model.endpoint = "ws://127.0.0.1:19004/v1/faces";
  return model;
}

models::ModelConfig ObjectModel(bool enabled = true,
                                models::Capability capability = models::Capability::kDetection,
                                models::Transport transport = models::Transport::kHttp) {
  models::ModelConfig model;
  model.id = "objects";
  model.enabled = enabled;
  model.capability = capability;
  model.transport = transport;
  model.endpoint = transport == models::Transport::kHttp
                       ? "http://127.0.0.1:19005/v1/detect"
                       : "ws://127.0.0.1:19005/v1/detect";
  return model;
}

hardware_runtime::Binding DemoBinding() {
  return hardware_runtime::LoadBinding(std::string(EOS_SOURCE_ETC) + "/backends/demo.toml", "demo", eos::backends::BuildBuiltinBackendCatalog());
}

std::string ValidationError(std::vector<models::ModelConfig> config) {
  config.push_back(FaceModel());
  const hardware_runtime::Binding binding = DemoBinding();
  models::Runtime models = ModelRuntime(std::move(config));
  try {
    ValidateAdvancedPrimitiveModels(binding, models);
  } catch (const std::runtime_error& error) {
    return error.what();
  }
  return {};
}

TEST(AdvancedPrimitiveModelsTest, EnabledExactCapabilityIsAdmitted) {
  const hardware_runtime::Binding binding = DemoBinding();
  models::Runtime models = ModelRuntime({TtsModel(), FaceModel()});
  EXPECT_NO_THROW(ValidateAdvancedPrimitiveModels(binding, models));
}

TEST(AdvancedPrimitiveModelsTest, MissingModelRefusesStartup) {
  const std::string error = ValidationError({});
  EXPECT_NE(error.find("advanced primitive 'speaker.speak@1.0.0'"), std::string::npos);
  EXPECT_NE(error.find("model 'audio8' is not configured"), std::string::npos);
}

TEST(AdvancedPrimitiveModelsTest, DisabledModelRefusesStartup) {
  const std::string error = ValidationError({TtsModel(false)});
  EXPECT_NE(error.find("is disabled; set enabled = true to launch it"), std::string::npos);
}

TEST(AdvancedPrimitiveModelsTest, WrongCapabilityRefusesStartup) {
  const std::string error = ValidationError({TtsModel(true, models::Capability::kAsr)});
  EXPECT_NE(error.find("must have capability audio.synthesis"), std::string::npos);
}

TEST(AdvancedPrimitiveModelsTest, FaceRegistrationRequiresWebSocketModel) {
  models::ModelConfig face = FaceModel();
  face.transport = models::Transport::kHttp;
  face.endpoint = "http://127.0.0.1:19004/v1/faces";
  const hardware_runtime::Binding binding = DemoBinding();
  models::Runtime models = ModelRuntime({TtsModel(), face});
  try {
    ValidateAdvancedPrimitiveModels(binding, models);
    FAIL() << "expected incompatible face registration binding";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("face.register@1.0.0"), std::string::npos);
    EXPECT_NE(std::string(error.what()).find("websocket transport"), std::string::npos);
  }
}

TEST(CameraObjectModelTest, NoConfigurationDeclaresNoProducer) {
  models::Runtime models = ModelRuntime({});
  EXPECT_FALSE(ValidateCameraObjectModel(models, ""));
}

TEST(CameraObjectModelTest, AcceptsEnabledDetectionOverHttpOrWebSocket) {
  for (const auto transport : {models::Transport::kHttp, models::Transport::kWebSocket}) {
    models::Runtime models = ModelRuntime({ObjectModel(true, models::Capability::kDetection,
                                                        transport)});
    EXPECT_TRUE(ValidateCameraObjectModel(models, "objects"));
  }
}

TEST(CameraObjectModelTest, RejectsMissingDisabledAndWrongCapability) {
  models::Runtime missing = ModelRuntime({});
  EXPECT_THROW(ValidateCameraObjectModel(missing, "objects"), std::runtime_error);
  models::Runtime disabled = ModelRuntime({ObjectModel(false)});
  EXPECT_THROW(ValidateCameraObjectModel(disabled, "objects"), std::runtime_error);
  models::Runtime wrong = ModelRuntime({ObjectModel(true, models::Capability::kHuman)});
  EXPECT_THROW(ValidateCameraObjectModel(wrong, "objects"), std::runtime_error);
}

}  // namespace
}  // namespace eos::runtime::init

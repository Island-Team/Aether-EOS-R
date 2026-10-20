#include "src/eos/runtime/models/runtime.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "src/eos/lib/log.h"

namespace eos::runtime::models {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

class OneShotServer {
 public:
  explicit OneShotServer(std::string body, http::status status = http::status::ok,
                         std::chrono::milliseconds delay = std::chrono::milliseconds(0),
                         std::string content_type = "application/json")
      : acceptor_(io_, {asio::ip::make_address("127.0.0.1"), 0}),
        response_body_(std::move(body)),
        response_status_(status),
        delay_(delay),
        content_type_(std::move(content_type)),
        worker_([this] { Serve(); }) {}

  ~OneShotServer() {
    if (worker_.joinable()) worker_.join();
  }

  std::uint16_t port() const { return acceptor_.local_endpoint().port(); }

  void Join() {
    if (worker_.joinable()) worker_.join();
  }

  const http::request<http::string_body>& request() const { return request_; }

  bool WaitForRequest() {
    std::unique_lock<std::mutex> lock(request_mutex_);
    return request_ready_.wait_for(lock, std::chrono::seconds(2),
                                   [this] { return request_received_; });
  }

 private:
  void Serve() {
    tcp::socket socket(io_);
    acceptor_.accept(socket);
    beast::flat_buffer buffer;
    http::read(socket, buffer, request_);
    {
      std::lock_guard<std::mutex> lock(request_mutex_);
      request_received_ = true;
    }
    request_ready_.notify_all();
    std::this_thread::sleep_for(delay_);
    http::response<http::string_body> response{response_status_, 11};
    response.set(http::field::content_type, content_type_);
    response.body() = response_body_;
    response.prepare_payload();
    beast::error_code ignored;
    http::write(socket, response, ignored);
    socket.shutdown(tcp::socket::shutdown_both, ignored);
  }

  asio::io_context io_;
  tcp::acceptor acceptor_;
  std::string response_body_;
  http::status response_status_;
  std::chrono::milliseconds delay_;
  std::string content_type_;
  http::request<http::string_body> request_;
  std::mutex request_mutex_;
  std::condition_variable request_ready_;
  bool request_received_{false};
  std::thread worker_;
};

class SequenceServer {
 public:
  explicit SequenceServer(std::vector<std::string> bodies)
      : acceptor_(io_, {asio::ip::make_address("127.0.0.1"), 0}),
        bodies_(std::move(bodies)),
        worker_([this] { Serve(); }) {}
  ~SequenceServer() {
    if (worker_.joinable()) worker_.join();
  }
  std::uint16_t port() const { return acceptor_.local_endpoint().port(); }
  bool WaitForRequests(std::size_t count) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, std::chrono::seconds(2),
                             [this, count] { return requests_ >= count; });
  }

 private:
  void Serve() {
    for (const std::string& body : bodies_) {
      tcp::socket socket(io_);
      acceptor_.accept(socket);
      beast::flat_buffer buffer;
      http::request<http::string_body> request;
      http::read(socket, buffer, request);
      http::response<http::string_body> response{http::status::ok, 11};
      response.set(http::field::content_type, "application/json");
      response.body() = body;
      response.prepare_payload();
      beast::error_code ignored;
      http::write(socket, response, ignored);
      socket.shutdown(tcp::socket::shutdown_both, ignored);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        ++requests_;
      }
      changed_.notify_all();
    }
  }

  asio::io_context io_;
  tcp::acceptor acceptor_;
  std::vector<std::string> bodies_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::size_t requests_{0};
  std::thread worker_;
};

class WebSocketServer {
 public:
  using Reply = std::pair<std::string, std::chrono::milliseconds>;

  explicit WebSocketServer(std::vector<std::vector<Reply>> sessions)
      : acceptor_(io_, {asio::ip::make_address("127.0.0.1"), 0}),
        sessions_(std::move(sessions)),
        worker_([this] { Serve(); }) {}
  ~WebSocketServer() {
    if (worker_.joinable()) worker_.join();
  }
  std::uint16_t port() const { return acceptor_.local_endpoint().port(); }
  std::size_t connections() const { return connections_.load(); }
  const std::vector<std::string>& requests() const { return requests_; }

 private:
  void Serve() {
    for (const auto& session : sessions_) {
      tcp::socket socket(io_);
      acceptor_.accept(socket);
      websocket::stream<tcp::socket> stream(std::move(socket));
      stream.accept();
      ++connections_;
      for (const auto& [response, delay] : session) {
        beast::flat_buffer buffer;
        beast::error_code error;
        stream.read(buffer, error);
        if (error) break;
        requests_.push_back(beast::buffers_to_string(buffer.data()));
        std::this_thread::sleep_for(delay);
        stream.text(true);
        stream.write(asio::buffer(response), error);
        if (error) break;
      }
      beast::error_code ignored;
      stream.close(websocket::close_code::normal, ignored);
    }
  }

  asio::io_context io_;
  tcp::acceptor acceptor_;
  std::vector<std::vector<Reply>> sessions_;
  std::atomic<std::size_t> connections_{0};
  std::vector<std::string> requests_;
  std::thread worker_;
};

std::string Endpoint(const OneShotServer& server, const std::string& target = "/infer") {
  return "http://127.0.0.1:" + std::to_string(server.port()) + target;
}

ModelConfig Model(std::string id, Capability capability, std::string endpoint) {
  ModelConfig model;
  model.id = std::move(id);
  model.capability = capability;
  model.endpoint = std::move(endpoint);
  model.request_contract = "request";
  model.response_contract = "response";
  return model;
}

ContractValidator AcceptAll() {
  return [](const std::string&, const nlohmann::json&) {};
}

ModelHealth WaitForHealth(Runtime& runtime, const std::string& id) {
  for (int attempt = 0; attempt < 200; ++attempt) {
    const std::optional<ModelHealth> health = runtime.Health(id);
    if (health && health->status != HealthStatus::kChecking) return *health;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return runtime.Health(id).value();
}

TEST(ModelsConfigTest, MissingPerceptionSectionProducesEmptyRegistry) {
  EXPECT_TRUE(ParseConfig("[gateway]\nhost = \"127.0.0.1\"\n", "test.toml").empty());
}

TEST(ModelsConfigTest, ArmGraspPlannerUsesVersionedWebSocketContracts) {
  const auto models = ParseConfig(R"toml(
    [[perception.model]]
    id = "arm-grasp-planner"
    enabled = false
    capability = "arm.grasp_plan"
    transport = "websocket"
    endpoint = "ws://127.0.0.1:18766/v1/plan"
    health_endpoint = "http://127.0.0.1:18767/health"
    adapter = "grasp-plan-ws-v1"
  )toml", "test.toml");
  ASSERT_EQ(models.size(), 1u);
  EXPECT_EQ(models[0].capability, Capability::kArmGraspPlan);
  EXPECT_EQ(models[0].request_contract, "perception/model/grasp_plan_request.v1.json");
  EXPECT_EQ(models[0].response_contract, "perception/model/grasp_plan_response.v1.json");
}

TEST(ModelsConfigTest, ParsesSortsAndDerivesCapabilityContracts) {
  const std::vector<ModelConfig> models = ParseConfig(R"toml(
    [[perception.model]]
    id = "pose-z"
    enabled = false
    capability = "vision.human_pose"
    endpoint = "http://127.0.0.1:18014/v1/pose"
    wire_format = "json"
    timeout_ms = 900

    [[perception.model]]
    id = "asr-a"
    capability = "audio.transcription"
    endpoint = "http://127.0.0.1:18015/v1/audio/transcriptions"
    health_endpoint = "http://127.0.0.1:18015/health"
    api_key_env = "ASR_KEY"
    provider_model_id = "whisper-1"
    max_request_bytes = 1024
    max_response_bytes = 2048
  )toml",
                                                      "test.toml");

  ASSERT_EQ(models.size(), 2u);
  EXPECT_EQ(models[0].id, "asr-a");
  EXPECT_TRUE(models[0].enabled);
  EXPECT_EQ(models[0].request_contract, "perception/model/asr_request.v1.json");
  EXPECT_EQ(models[0].response_contract, "perception/model/asr_response.v1.json");
  EXPECT_EQ(models[0].provider_model_id, "whisper-1");
  EXPECT_EQ(models[0].max_request_bytes, 1024u);
  EXPECT_EQ(models[0].max_response_bytes, 2048u);
  EXPECT_EQ(models[1].wire_format, WireFormat::kJson);
  EXPECT_FALSE(models[1].enabled);
  EXPECT_EQ(models[1].timeout, std::chrono::milliseconds(900));
  EXPECT_EQ(models[1].capability, Capability::kHumanPose);
  EXPECT_EQ(models[1].request_contract, "perception/model/human_pose_request.v1.json");
  EXPECT_EQ(models[1].response_contract, "perception/model/human_pose_response.v1.json");
}

TEST(ModelsConfigTest, CommittedConfigSelectsReferenceClients) {
  std::ifstream input(std::string(EOS_SOURCE_ETC) + "/eos.example.toml");
  ASSERT_TRUE(input);
  std::ostringstream text;
  text << input.rdbuf();
  const std::vector<ModelConfig> models = ParseConfig(text.str(), "eos.example.toml");
  ASSERT_EQ(models.size(), 2u);
  const auto face = std::find_if(models.begin(), models.end(), [](const ModelConfig& model) {
    return model.id == "arcface-register";
  });
  ASSERT_NE(face, models.end());
  EXPECT_TRUE(face->enabled);
  EXPECT_EQ(face->capability, Capability::kFace);
  EXPECT_EQ(face->transport, Transport::kWebSocket);
  EXPECT_EQ(face->endpoint, "ws://127.0.0.1:18080/v1/faces/recognize/stream");
  EXPECT_EQ(face->adapter, "arcface-stream-v1");

  const auto speech = std::find_if(models.begin(), models.end(), [](const ModelConfig& model) {
    return model.id == "audio8";
  });
  ASSERT_NE(speech, models.end());
  EXPECT_TRUE(speech->enabled);
  EXPECT_EQ(speech->capability, Capability::kTts);
  EXPECT_EQ(speech->transport, Transport::kHttp);
  EXPECT_EQ(speech->endpoint, "http://127.0.0.1:18081/v1/audio/speech");
  EXPECT_EQ(speech->adapter, "audio8-speech-v1");
}

TEST(ModelsConfigTest, TestConfigurationUsesLoopbackProviders) {
  std::ifstream input(std::string(EOS_SOURCE_ETC) + "/test/eos.test.toml");
  ASSERT_TRUE(input);
  std::ostringstream text;
  text << input.rdbuf();
  const std::vector<ModelConfig> models = ParseConfig(text.str(), "eos.test.toml");
  ASSERT_EQ(models.size(), 4u);

  const std::vector<std::pair<std::string, Capability>> loopback = {
      {"local-asr", Capability::kAsr},
      {"audio8", Capability::kTts},
      {"local-vad", Capability::kVad}};
  for (const auto& [id, capability] : loopback) {
    const auto found = std::find_if(models.begin(), models.end(),
                                    [&id](const ModelConfig& model) { return model.id == id; });
    ASSERT_NE(found, models.end()) << id;
    EXPECT_EQ(found->wire_format, WireFormat::kJson) << id;
    EXPECT_EQ(found->capability, capability) << id;
    EXPECT_FALSE(found->endpoint.empty()) << id;
    EXPECT_TRUE(found->api_key_env.empty()) << id;
  }
  EXPECT_NE(std::find_if(models.begin(), models.end(),
                         [](const ModelConfig& model) {
                           return model.id == "test-asr" && !model.health_endpoint.empty();
                         }),
            models.end());
}

TEST(ModelsConfigTest, ParsesWebSocketTransportAndRejectsSchemeMismatch) {
  const std::vector<ModelConfig> models = ParseConfig(R"toml(
    [[perception.model]]
    id = "pose-stream"
    capability = "vision.pose"
    transport = "websocket"
    endpoint = "ws://127.0.0.1:18014/v1/stream/posture"
  )toml",
                                                      "websocket.toml");
  ASSERT_EQ(models.size(), 1u);
  EXPECT_EQ(models[0].transport, Transport::kWebSocket);
  EXPECT_EQ(models[0].wire_format, WireFormat::kJson);

  EXPECT_THROW(ParseConfig(R"toml(
    [[perception.model]]
    id = "bad-stream"
    capability = "vision.pose"
    transport = "websocket"
    endpoint = "http://127.0.0.1:18014/v1/stream/posture"
  )toml",
                           "bad-websocket.toml"),
               std::runtime_error);
  EXPECT_THROW(ParseConfig(R"toml(
    [[perception.model]]
    id = "bad-multipart-stream"
    capability = "vision.pose"
    transport = "websocket"
    endpoint = "ws://127.0.0.1:18014/v1/stream/posture"
    wire_format = "multipart"
  )toml",
                           "bad-websocket-wire.toml"),
               std::runtime_error);
  EXPECT_THROW(ParseConfig(R"toml(
    [[perception.model]]
    id = "bad-audio-stream"
    capability = "audio.vad"
    transport = "websocket"
    endpoint = "ws://127.0.0.1:18013/vad"
  )toml",
                           "bad-websocket-capability.toml"),
               std::runtime_error);
  EXPECT_THROW(ParseConfig(R"toml(
    [[perception.model]]
    id = "bad-auth-stream"
    capability = "vision.face"
    transport = "websocket"
    endpoint = "ws://127.0.0.1:18017/v1/faces/recognize/stream"
    api_key_env = "MODEL_TOKEN"
  )toml",
                           "bad-websocket-auth.toml"),
               std::runtime_error);
}

TEST(ModelsConfigTest, RequiresAndBoundsHttpHealthConfiguration) {
  const std::vector<ModelConfig> without_health = ParseConfig(R"toml(
    [[perception.model]]
    id = "vad"
    capability = "audio.vad"
    endpoint = "http://127.0.0.1:18013/vad"
  )toml",
                                                              "missing-health.toml");
  ASSERT_EQ(without_health.size(), 1u);
  EXPECT_TRUE(without_health.front().health_endpoint.empty());

  const std::vector<ModelConfig> models = ParseConfig(R"toml(
    [[perception.model]]
    id = "vad"
    capability = "audio.vad"
    endpoint = "http://127.0.0.1:18013/vad"
    health_endpoint = "http://127.0.0.1:18013/health"
    health_interval_ms = 250
    health_timeout_ms = 75
  )toml",
                                                      "health.toml");
  ASSERT_EQ(models.size(), 1u);
  EXPECT_EQ(models[0].health_endpoint, "http://127.0.0.1:18013/health");
  EXPECT_EQ(models[0].health_interval, std::chrono::milliseconds(250));
  EXPECT_EQ(models[0].health_timeout, std::chrono::milliseconds(75));

  EXPECT_THROW(ParseConfig(R"toml(
    [[perception.model]]
    id = "vad"
    capability = "audio.vad"
    endpoint = "http://127.0.0.1:18013/vad"
    health_endpoint = "http://127.0.0.1:18013/health"
    health_interval_ms = 3600001
  )toml",
                           "health-interval.toml"),
               std::runtime_error);
}

TEST(ModelsConfigTest, AcceptsWebSocketModelEndpointButNotWebSocketHealth) {
  const auto models = ParseConfig(R"toml(
    [[perception.model]]
    id = "pose"
    capability = "vision.pose"
    transport = "websocket"
    endpoint = "ws://127.0.0.1:18013/pose"
  )toml",
                                   "websocket.toml");
  ASSERT_EQ(models.size(), 1u);
  EXPECT_EQ(models.front().endpoint, "ws://127.0.0.1:18013/pose");
  EXPECT_THROW(ParseConfig(R"toml(
    [[perception.model]]
    id = "pose"
    capability = "vision.pose"
    transport = "websocket"
    endpoint = "ws://127.0.0.1:18013/pose"
    health_endpoint = "ws://127.0.0.1:18013/health"
  )toml",
                           "websocket-health.toml"),
               std::runtime_error);
}

TEST(ModelsConfigTest, RejectsDuplicateIdUnknownKeyAndHttps) {
  EXPECT_THROW(ParseConfig(R"toml(
    [[perception.model]]
    id = "vad"
    enabled = "yes"
    capability = "audio.vad"
    endpoint = "http://127.0.0.1/vad"
  )toml",
                           "enabled.toml"),
               std::runtime_error);
  EXPECT_THROW(ParseConfig(R"toml(
    [[perception.model]]
    id = "same"
    enabled = false
    capability = "audio.vad"
    endpoint = "http://127.0.0.1/vad"
    [[perception.model]]
    id = "same"
    capability = "audio.vad"
    endpoint = "http://127.0.0.1/vad2"
  )toml",
                           "duplicate.toml"),
               std::runtime_error);
  EXPECT_THROW(ParseConfig(R"toml(
    [[perception.model]]
    id = "vad"
    capability = "audio.vad"
    endpoint = "http://127.0.0.1/vad"
    retry = true
  )toml",
                           "unknown.toml"),
               std::runtime_error);
  EXPECT_THROW(ParseConfig(R"toml(
    [[perception.model]]
    id = "vad"
    capability = "audio.vad"
    endpoint = "https://localhost/vad"
  )toml",
                           "https.toml"),
               std::runtime_error);
  EXPECT_THROW(ParseConfig(R"toml(
    [[perception.model]]
    id = "vad"
    capability = "audio.vad"
    endpoint = "http://user@localhost/vad"
  )toml",
                           "userinfo.toml"),
               std::runtime_error);
  EXPECT_THROW(ParseConfig(R"toml(
    [[perception.model]]
    id = "vad"
    capability = "audio.vad"
    endpoint = "http://localhost/vad#fragment"
  )toml",
                           "fragment.toml"),
               std::runtime_error);
  EXPECT_THROW(ParseConfig(R"toml(
    [[perception.model]]
    id = "vad"
    capability = "audio.vad"
    endpoint = "http://models.example/vad"
  )toml",
                           "dns.toml"),
               std::runtime_error);
  EXPECT_THROW(ParseConfig(R"toml(
    [[perception.model]]
    id = "vad"
    capability = "audio.vad"
    endpoint = "http://127.0.0.1/bad path"
  )toml",
                           "target.toml"),
               std::runtime_error);
  EXPECT_THROW(ParseConfig(R"toml(
    [[perception.model]]
    id = "vad"
    capability = "audio.vad"
    endpoint = "http://127.0.0.1/bad%ZZ"
  )toml",
                           "escape.toml"),
               std::runtime_error);
  EXPECT_THROW(ParseConfig(R"toml(
    [[perception.model]]
    id = "vad"
    capability = "audio.vad"
    endpoint = "http://127.0.0.1/vad"
    file_field = "bad\\name"
  )toml",
                           "multipart.toml"),
               std::runtime_error);
}

TEST(ModelsRuntimeTest, UnknownModelReturnsNotFound) {
  Runtime runtime;
  EXPECT_EQ(runtime.Call("missing", nlohmann::json::object()).status,
            CallResult::Status::kNotFound);
}

TEST(ModelsRuntimeTest, LoadsPrivateSchemasWhenNoValidatorOverrideIsSupplied) {
  ModelConfig model = Model("asr", Capability::kAsr, "http://127.0.0.1:1/v1/audio/transcriptions");
  model.request_contract = "perception/model/asr_request.v1.json";
  model.response_contract = "perception/model/asr_response.v1.json";
  Runtime runtime({model});

  const CallResult result = runtime.Call("asr", nlohmann::json::object());
  EXPECT_EQ(result.status, CallResult::Status::kInvalidArgument);
  EXPECT_FALSE(result.message.empty());
}

TEST(ModelsRuntimeTest, ChatCompletionSchemaAllowsOnlyBoundedNonStreamingTextChat) {
  OneShotServer server(R"json({"text":"hello"})json");
  ModelConfig model = Model("chat", Capability::kChatCompletion, Endpoint(server));
  model.request_contract = "perception/model/chat_completion_request.v1.json";
  model.response_contract = "perception/model/chat_completion_response.v1.json";
  Runtime runtime({model});

  const nlohmann::json minimal{{"messages", {{{"role", "user"}, {"content", "Hello"}}}}};
  const CallResult completed = runtime.Call("chat", minimal);
  ASSERT_EQ(completed.status, CallResult::Status::kCompleted) << completed.message;
  EXPECT_EQ(completed.response, (nlohmann::json{{"text", "hello"}}));

  nlohmann::json streaming = minimal;
  streaming["stream"] = true;
  EXPECT_EQ(runtime.Call("chat", streaming).status, CallResult::Status::kInvalidArgument);
  nlohmann::json tools = minimal;
  tools["tools"] = nlohmann::json::array();
  EXPECT_EQ(runtime.Call("chat", tools).status, CallResult::Status::kInvalidArgument);
  nlohmann::json too_many_messages = minimal;
  too_many_messages["messages"] = nlohmann::json::array();
  for (int index = 0; index < 9; ++index) {
    too_many_messages["messages"].push_back({{"role", "user"}, {"content", "Hello"}});
  }
  EXPECT_EQ(runtime.Call("chat", too_many_messages).status, CallResult::Status::kInvalidArgument);
  nlohmann::json long_content = minimal;
  long_content["messages"][0]["content"] = std::string(4097, 'a');
  EXPECT_EQ(runtime.Call("chat", long_content).status, CallResult::Status::kInvalidArgument);
  nlohmann::json too_many_tokens = minimal;
  too_many_tokens["max_tokens"] = 128001;
  EXPECT_EQ(runtime.Call("chat", too_many_tokens).status, CallResult::Status::kInvalidArgument);

  OneShotServer oversized_server("{\"text\":\"" + std::string(4097, 'a') + "\"}");
  model.endpoint = Endpoint(oversized_server);
  Runtime oversized_runtime({model});
  EXPECT_EQ(oversized_runtime.Call("chat", minimal).status, CallResult::Status::kProtocolError);
}

TEST(ModelsRuntimeTest, VlmSchemaAllowsTextAndOptionalJpeg) {
  OneShotServer server(R"json({"text":"hello"})json");
  ModelConfig model = Model("vlm", Capability::kVlm, Endpoint(server));
  model.request_contract = "perception/model/vlm_request.v1.json";
  model.response_contract = "perception/model/vlm_response.v1.json";
  Runtime runtime({model});

  const nlohmann::json text_request{{"messages", {{{"role", "user"}, {"content", "Hello"}}}}};
  const auto completed = runtime.Call("vlm", text_request);
  ASSERT_EQ(completed.status, CallResult::Status::kCompleted) << completed.message;
  EXPECT_EQ(completed.response.at("text"), "hello");

  auto image_request = text_request;
  image_request["input"] = {{"data_b64", "AAAA"}, {"media_type", "image/jpeg"}};
  EXPECT_NE(runtime.Call("vlm", image_request).status, CallResult::Status::kInvalidArgument);
  image_request["input"]["media_type"] = "image/png";
  EXPECT_EQ(runtime.Call("vlm", image_request).status, CallResult::Status::kInvalidArgument);
  image_request = text_request;
  image_request["input"] = {{"data_b64", "bad"}, {"media_type", "image/jpeg"}};
  EXPECT_EQ(runtime.Call("vlm", image_request).status, CallResult::Status::kInvalidArgument);
  auto streaming = text_request;
  streaming["stream"] = true;
  EXPECT_EQ(runtime.Call("vlm", streaming).status, CallResult::Status::kInvalidArgument);
}

TEST(ModelsRuntimeTest, AcceptsMinimalAsrAndRejectsUnsupportedStreaming) {
  OneShotServer server(R"json({"text":"hello"})json");
  ModelConfig model = Model("asr", Capability::kAsr, Endpoint(server));
  model.request_contract = "perception/model/asr_request.v1.json";
  model.response_contract = "perception/model/asr_response.v1.json";
  Runtime runtime({model});

  const nlohmann::json minimal{{"input", {{"data_b64", "AAAA"}}}, {"stream", false}};
  const CallResult completed = runtime.Call("asr", minimal);
  ASSERT_EQ(completed.status, CallResult::Status::kCompleted) << completed.message;

  nlohmann::json streaming = minimal;
  streaming["stream"] = true;
  const CallResult unsupported = runtime.Call("asr", streaming);
  EXPECT_EQ(unsupported.status, CallResult::Status::kInvalidArgument);
  EXPECT_NE(unsupported.message.find("does not support streaming"), std::string::npos);

  nlohmann::json invalid = minimal;
  invalid["stream"] = "yes";
  EXPECT_EQ(runtime.Call("asr", invalid).status, CallResult::Status::kInvalidArgument);
}

TEST(ModelsRuntimeTest, RejectsUnsupportedVadAndTtsStreaming) {
  for (const auto& [id, capability, request] :
       std::vector<std::tuple<std::string, Capability, nlohmann::json>>{
           {"vad", Capability::kVad, {{"input", {{"data_b64", "AAAA"}}}, {"stream", true}}},
           {"tts", Capability::kTts, {{"text", "hello"}, {"stream", true}}}}) {
    Runtime runtime({Model(id, capability, "http://127.0.0.1:1/infer")}, AcceptAll());
    const CallResult result = runtime.Call(id, request);
    EXPECT_EQ(result.status, CallResult::Status::kInvalidArgument);
    EXPECT_NE(result.message.find("does not support streaming"), std::string::npos);
  }
}

TEST(ModelsRuntimeTest, NonCanonicalAdapterRequiresFactory) {
  ModelConfig model = Model("tts", Capability::kTts, "http://127.0.0.1:1/infer");
  model.adapter = "audio8-speech-v1";
  EXPECT_THROW(Runtime({model}, AcceptAll()), std::runtime_error);
}

TEST(ModelsRuntimeTest, HealthWorkerNormalizesProviderResponse) {
  OneShotServer server(R"json({
    "interface_version":"1.0",
    "status":"degraded",
    "service":{"name":"speech-service","version":"2.1"},
    "model":{"id":"whisper-1","version":"2026-09"},
    "capabilities":["audio.transcription"],
    "message":"warming"
  })json");
  ModelConfig model = Model("asr", Capability::kAsr, "http://127.0.0.1:1/infer");
  model.provider_model_id = "whisper-1";
  model.health_endpoint = Endpoint(server, "/health");
  model.health_interval = std::chrono::hours(1);
  Runtime runtime({model}, AcceptAll());

  ASSERT_TRUE(server.WaitForRequest());
  server.Join();
  std::optional<ModelHealth> health;
  for (int attempt = 0; attempt < 100; ++attempt) {
    health = runtime.Health("asr");
    if (health && health->status != HealthStatus::kChecking) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(health.has_value());
  EXPECT_EQ(health->status, HealthStatus::kDegraded);
  EXPECT_EQ(health->service_name, "speech-service");
  EXPECT_EQ(health->service_version, "2.1");
  EXPECT_EQ(health->provider_model_id, "whisper-1");
  EXPECT_EQ(health->model_version, "2026-09");
  EXPECT_EQ(health->message, "warming");
  EXPECT_EQ(server.request().method(), http::verb::get);
  EXPECT_EQ(server.request().target(), "/health");
}

TEST(ModelsRuntimeTest, ShutdownCancelsActiveHealthProbe) {
  OneShotServer server(
      R"({"status":"ready","interface_version":"1.0","service":{"name":"test","version":"1"},"model":{"id":"asr","version":"1"},"capabilities":["audio.transcription"]})",
      http::status::ok, std::chrono::seconds(1));
  ModelConfig model = Model("asr", Capability::kAsr, Endpoint(server));
  model.health_endpoint = Endpoint(server, "/health");
  model.health_interval = std::chrono::hours(1);
  model.health_timeout = std::chrono::seconds(30);

  auto runtime = std::make_unique<Runtime>(std::vector<ModelConfig>{model}, AcceptAll());
  ASSERT_TRUE(server.WaitForRequest());
  const auto started = std::chrono::steady_clock::now();
  runtime.reset();
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(500));
}

TEST(ModelsRuntimeTest, TtsHealthResponseIsDecodedAsJson) {
  OneShotServer server(R"json({
    "interface_version":"1.0",
    "status":"ready",
    "service":{"name":"speech-service","version":"2.1"},
    "model":{"id":"audio8","version":"2026-09"},
    "capabilities":["audio.synthesis"]
  })json");
  ModelConfig model = Model("tts", Capability::kTts, "http://127.0.0.1:1/infer");
  model.provider_model_id = "audio8";
  model.health_endpoint = Endpoint(server, "/health");
  model.health_interval = std::chrono::hours(1);
  Runtime runtime({model}, AcceptAll());

  ASSERT_TRUE(server.WaitForRequest());
  server.Join();
  const ModelHealth health = WaitForHealth(runtime, "tts");
  EXPECT_EQ(health.status, HealthStatus::kReady);
  EXPECT_EQ(health.provider_model_id, "audio8");
}

TEST(ModelsRuntimeTest, InvalidHealthResponseBecomesUnavailable) {
  OneShotServer server(R"json({
    "interface_version":"1.0",
    "status":"ready",
    "service":{"name":"vision","version":"1"},
    "model":{"id":"wrong-model","version":"1"},
    "capabilities":["vision.pose"]
  })json");
  ModelConfig model = Model("detector", Capability::kDetection, "http://127.0.0.1:1/infer");
  model.provider_model_id = "expected-model";
  model.health_endpoint = Endpoint(server, "/health");
  model.health_interval = std::chrono::hours(1);
  Runtime runtime({model}, AcceptAll());

  ASSERT_TRUE(server.WaitForRequest());
  server.Join();
  std::optional<ModelHealth> health;
  for (int attempt = 0; attempt < 100; ++attempt) {
    health = runtime.Health("detector");
    if (health && health->status != HealthStatus::kChecking) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(health.has_value());
  EXPECT_EQ(health->status, HealthStatus::kUnavailable);
  EXPECT_NE(health->message.find("configured capability"), std::string::npos);
}

TEST(ModelsRuntimeTest, HealthRejectsMalformedVersionIdentityAndTimeout) {
  const auto CheckUnavailable = [](std::string body, std::chrono::milliseconds delay,
                                   std::string expected) {
    OneShotServer server(std::move(body), http::status::ok, delay);
    ModelConfig model = Model("asr", Capability::kAsr, "http://127.0.0.1:1/infer");
    model.provider_model_id = "expected";
    model.health_endpoint = Endpoint(server, "/health");
    model.health_interval = std::chrono::hours(1);
    model.health_timeout = std::chrono::milliseconds(20);
    Runtime runtime({model}, AcceptAll());
    ASSERT_TRUE(server.WaitForRequest());
    const ModelHealth health = WaitForHealth(runtime, "asr");
    EXPECT_EQ(health.status, HealthStatus::kUnavailable);
    EXPECT_NE(health.message.find(expected), std::string::npos) << health.message;
    server.Join();
  };
  CheckUnavailable("{bad", std::chrono::milliseconds(0), "invalid response");
  CheckUnavailable(
      R"({"interface_version":"2.0","status":"ready","service":{"name":"s","version":"1"},"model":{"id":"expected","version":"1"},"capabilities":["audio.transcription"]})",
      std::chrono::milliseconds(0), "interface_version");
  CheckUnavailable(
      R"({"interface_version":"1.0","status":"ready","service":{"name":"s","version":"1"},"model":{"id":"wrong","version":"1"},"capabilities":["audio.transcription"]})",
      std::chrono::milliseconds(0), "identity");
  CheckUnavailable("{}", std::chrono::milliseconds(100), "exceeded");
}

TEST(ModelsRuntimeTest, HealthRecoversAndSupportsConcurrentReaders) {
  SequenceServer server(
      {"{bad",
       R"({"interface_version":"1.0","status":"ready","service":{"name":"s","version":"1"},"model":{"id":"expected","version":"1"},"capabilities":["audio.transcription"]})"});
  ModelConfig model = Model("asr", Capability::kAsr, "http://127.0.0.1:1/infer");
  model.provider_model_id = "expected";
  model.health_endpoint = "http://127.0.0.1:" + std::to_string(server.port()) + "/health";
  model.health_interval = std::chrono::milliseconds(200);
  Runtime runtime({model}, AcceptAll());
  ASSERT_TRUE(server.WaitForRequests(1));
  EXPECT_EQ(WaitForHealth(runtime, "asr").status, HealthStatus::kUnavailable);
  std::vector<std::future<std::size_t>> reads;
  for (int i = 0; i < 8; ++i) {
    reads.push_back(
        std::async(std::launch::async, [&runtime] { return runtime.HealthAll().size(); }));
  }
  for (auto& read : reads) EXPECT_EQ(read.get(), 1u);
  ASSERT_TRUE(server.WaitForRequests(2));
  for (int attempt = 0; attempt < 200; ++attempt) {
    if (runtime.Health("asr")->status == HealthStatus::kReady) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(runtime.Health("asr")->status, HealthStatus::kReady);
}

TEST(ModelsRuntimeTest, DisabledModelIsHiddenAndNeverSubstituted) {
  ModelConfig disabled = Model("disabled-vad", Capability::kVad, "http://127.0.0.1:1/vad");
  disabled.enabled = false;
  ModelConfig enabled = Model("enabled-vad", Capability::kVad, "http://127.0.0.1:1/vad");
  Runtime runtime({disabled, enabled}, AcceptAll());

  ASSERT_EQ(runtime.Discover().size(), 1u);
  EXPECT_EQ(runtime.Discover()[0].id, "enabled-vad");
  EXPECT_EQ(runtime.Find("disabled-vad"), nullptr);
  EXPECT_EQ(runtime.Resolve("disabled-vad", Capability::kVad), ModelReferenceState::kDisabled);
  EXPECT_EQ(runtime.Resolve("disabled-vad", Capability::kAsr),
            ModelReferenceState::kWrongCapability);
  EXPECT_EQ(runtime.Resolve("missing", Capability::kVad), ModelReferenceState::kMissing);
  EXPECT_EQ(runtime.Resolve("enabled-vad", Capability::kVad), ModelReferenceState::kEnabled);
  EXPECT_EQ(runtime.Call("disabled-vad", nlohmann::json::object()).status,
            CallResult::Status::kNotFound);
}

TEST(ModelsRuntimeTest, UnifiedGemmaResolvesVlmAndPostureFromOneEntry) {
  ModelConfig gemma = Model("vla-gemma4", Capability::kVlm, "http://127.0.0.1:1/v1/chat/completions");
  gemma.adapter = "gemma4-unified-v1";
  struct NoopAdapter final : Adapter {
    nlohmann::json PrepareRequest(const ModelConfig&, const nlohmann::json& request) const override {
      return request;
    }
    nlohmann::json DecodeResponse(const ModelConfig&, std::string_view,
                                  std::string_view) const override { return {}; }
    void NormalizeResponse(const ModelConfig&, nlohmann::json*) const override {}
  };
  Runtime runtime({gemma}, AcceptAll(),
                  [](const ModelConfig&) { return std::make_shared<NoopAdapter>(); });
  EXPECT_EQ(runtime.Resolve("vla-gemma4", Capability::kVlm), ModelReferenceState::kEnabled);
  EXPECT_EQ(runtime.Resolve("vla-gemma4", Capability::kPostureClassification),
            ModelReferenceState::kEnabled);
  EXPECT_EQ(runtime.Resolve("vla-gemma4", Capability::kAsr),
            ModelReferenceState::kWrongCapability);
}

TEST(ModelsRuntimeTest, DisabledModelIdsStillParticipateInRuntimeDeduplication) {
  ModelConfig disabled = Model("same", Capability::kVad, "http://127.0.0.1:1/vad");
  disabled.enabled = false;
  ModelConfig enabled = Model("same", Capability::kVad, "http://127.0.0.1:2/vad");

  EXPECT_THROW(Runtime({disabled, enabled}, AcceptAll()), std::runtime_error);
}

TEST(ModelsRuntimeTest, SendsCanonicalAsrJsonWithBearerToken) {
  OneShotServer server(R"json({"text":"hello"})json");
  ModelConfig model = Model("asr", Capability::kAsr, Endpoint(server));
  model.provider_model_id = "whisper-1";
  model.api_key_env = "EOS_MODELS_TEST_KEY";
  ASSERT_EQ(setenv("EOS_MODELS_TEST_KEY", "secret", 1), 0);
  std::vector<std::string> validated;
  Runtime runtime({model}, [&validated](const std::string& contract, const nlohmann::json&) {
    validated.push_back(contract);
  });

  const nlohmann::json request = {
      {"input", {{"data_b64", "AQID"}, {"filename", "speech.wav"}, {"media_type", "audio/wav"}}},
      {"language", "en"},
      {"prompt", "hello"}};
  const CallResult result = runtime.Call("asr", request);
  server.Join();
  unsetenv("EOS_MODELS_TEST_KEY");

  EXPECT_EQ(result.status, CallResult::Status::kCompleted) << result.message;
  EXPECT_EQ(result.response.at("text"), "hello");
  EXPECT_EQ(server.request().target(), "/infer");
  EXPECT_EQ(server.request()[http::field::authorization], "Bearer secret");
  const nlohmann::json body = nlohmann::json::parse(server.request().body());
  EXPECT_EQ(body, request);
  EXPECT_EQ(validated, (std::vector<std::string>{"request", "response"}));
}

TEST(ModelsRuntimeTest, DefaultsOptionalMultipartAudioMetadata) {
  OneShotServer server(R"json({"speech":false,"probability":0.0,"segments":[]})json");
  ModelConfig model = Model("vad", Capability::kVad, Endpoint(server));
  model.wire_format = WireFormat::kMultipart;
  Runtime runtime({model}, AcceptAll());

  const CallResult result = runtime.Call("vad", {{"input", {{"data_b64", "AQID"}}}});
  server.Join();

  EXPECT_EQ(result.status, CallResult::Status::kCompleted) << result.message;
  EXPECT_NE(server.request().body().find("filename=\"audio.wav\""), std::string::npos);
  EXPECT_NE(server.request().body().find("Content-Type: audio/wav"), std::string::npos);
  EXPECT_NE(server.request().body().find("name=\"sample_rate_hz\""), std::string::npos);
  EXPECT_NE(server.request().body().find("name=\"channels\""), std::string::npos);
}

TEST(ModelsRuntimeTest, AcceptsMinimalImageInputAndDefaultsMultipartMetadata) {
  OneShotServer server(
      R"json({"objects":[{"label":"empty_box","class_id":1,"source_label":"shupian","source_class_id":7,"confidence":0.9,"bbox":[1,2,30,40]}],"object_count":1})json");
  ModelConfig model = Model("detector", Capability::kDetection, Endpoint(server));
  model.wire_format = WireFormat::kMultipart;
  model.request_contract = "perception/model/detection_request.v1.json";
  model.response_contract = "perception/model/detection_response.v1.json";
  Runtime runtime({model});

  const CallResult result = runtime.Call("detector", {{"input", {{"data_b64", "AQID"}}}});
  server.Join();

  EXPECT_EQ(result.status, CallResult::Status::kCompleted) << result.message;
  EXPECT_EQ(result.response.at("objects").at(0).at("source_label"), "shupian");
  EXPECT_NE(server.request().body().find("filename=\"image.jpg\""), std::string::npos);
  EXPECT_NE(server.request().body().find("Content-Type: image/jpeg"), std::string::npos);
}

TEST(ModelsRuntimeTest, EnforcesTrackedHumanAndCompleteHumanPoseSchemas) {
  OneShotServer human_server(
      R"json({"persons":[{"track_id":7,"bbox":[1,2,30,40],"confidence":0.9,"distance_m":2.5}],"person_count":1})json");
  ModelConfig human = Model("human", Capability::kHuman, Endpoint(human_server));
  human.request_contract = "perception/model/human_request.v1.json";
  human.response_contract = "perception/model/human_response.v1.json";
  Runtime human_runtime({human});
  EXPECT_EQ(human_runtime.Call("human", {{"input", {{"data_b64", "AAAA"}}}}).status,
            CallResult::Status::kCompleted);
  human_server.Join();

  OneShotServer incomplete_pose(
      R"json({"persons":[{"track_id":7,"bbox":[1,2,30,40],"confidence":0.9,"distance_m":null,"keypoints":[],"keypoint_scores":[],"pose":"stand","pose_confidence":0.9,"action":"none","action_confidence":0.0}],"person_count":1})json");
  ModelConfig pose = Model("human-pose", Capability::kHumanPose, Endpoint(incomplete_pose));
  pose.request_contract = "perception/model/human_pose_request.v1.json";
  pose.response_contract = "perception/model/human_pose_response.v1.json";
  Runtime pose_runtime({pose});
  const nlohmann::json request = {
      {"input", {{"data_b64", "AAAA"}}},
      {"persons", {{{"track_id", 7}, {"bbox", {1, 2, 30, 40}}, {"confidence", 0.9}}}}};
  EXPECT_EQ(pose_runtime.Call("human-pose", request).status,
            CallResult::Status::kProtocolError);
  incomplete_pose.Join();

  nlohmann::json keypoints = nlohmann::json::array();
  nlohmann::json keypoint_scores = nlohmann::json::array();
  for (int index = 0; index < 17; ++index) {
    keypoints.push_back({index, index + 1});
    keypoint_scores.push_back(0.8);
  }
  const nlohmann::json complete_response{
      {"persons",
       {{{"track_id", 7},
         {"bbox", {1, 2, 30, 40}},
         {"confidence", 0.9},
         {"distance_m", nullptr},
         {"keypoints", keypoints},
         {"keypoint_scores", keypoint_scores},
          {"pose", "stand"},
         {"pose_confidence", 0.9},
         {"action", "none"},
         {"action_confidence", 0.0}}}},
      {"person_count", 1}};
  OneShotServer complete_pose(complete_response.dump());
  pose.endpoint = Endpoint(complete_pose);
  Runtime complete_pose_runtime({pose});
  EXPECT_EQ(complete_pose_runtime.Call("human-pose", request).status,
            CallResult::Status::kCompleted);
  complete_pose.Join();
}

TEST(ModelsRuntimeTest, FaceV1SchemaStillAcceptsRegistration) {
  ModelConfig face = Model("face", Capability::kFace, "http://127.0.0.1:1/v1/faces");
  face.request_contract = "perception/model/face_request.v1.json";
  face.response_contract = "perception/model/face_response.v1.json";
  Runtime runtime({face});
  EXPECT_NE(runtime.Call("face", {{"operation", "register"},
                                  {"input", {{"data_b64", "AAAA"}}},
                                  {"name", "Alice"}})
                .status,
            CallResult::Status::kInvalidArgument);
}

TEST(ModelsRuntimeTest, CanonicalTtsDecodesJsonResponse) {
  OneShotServer server(
      R"json({"data_b64":"UklGRg==","media_type":"audio/wav","format":"wav"})json");
  Runtime runtime({Model("tts", Capability::kTts, Endpoint(server))}, AcceptAll());
  const CallResult result = runtime.Call("tts", {{"text", "hello"}});
  server.Join();
  EXPECT_EQ(result.status, CallResult::Status::kCompleted) << result.message;
  EXPECT_EQ(result.response.at("media_type"), "audio/wav");
}

TEST(ModelsRuntimeTest, EnforcesRequestAndResponseByteLimits) {
  ModelConfig request_model = Model("vad", Capability::kVad, "http://127.0.0.1:1/vad");
  request_model.max_request_bytes = 2;
  Runtime request_runtime({request_model}, AcceptAll());
  EXPECT_EQ(request_runtime.Call("vad", {{"long", "request"}}).status,
            CallResult::Status::kInvalidArgument);

  OneShotServer server("123");
  ModelConfig response_model = Model("vad", Capability::kVad, Endpoint(server));
  response_model.max_response_bytes = 2;
  Runtime response_runtime({response_model}, AcceptAll());
  EXPECT_EQ(response_runtime.Call("vad", {}).status, CallResult::Status::kProtocolError);
  server.Join();
}

TEST(ModelsRuntimeTest, RunsAsyncCallsInModuleOwnedWorkers) {
  OneShotServer server(R"json({"speech":false,"probability":0.0,"segments":[]})json");
  std::promise<std::string> validated;
  Runtime runtime({Model("vad", Capability::kVad, Endpoint(server))},
                  [&validated](const std::string& contract, const nlohmann::json&) {
                    if (contract == "request")
                      validated.set_value(eos::lib::CurrentCorrelationId());
                  });
  std::promise<std::pair<CallResult, std::string>> completed;
  {
    eos::lib::CorrelationScope correlation("async-model-call");
    ASSERT_TRUE(runtime.CallAsync("vad", {}, [&completed](CallResult result) {
      completed.set_value({std::move(result), eos::lib::CurrentCorrelationId()});
    }));
  }
  EXPECT_EQ(validated.get_future().get(), "async-model-call");
  const auto [result, completion_correlation] = completed.get_future().get();
  EXPECT_EQ(result.status, CallResult::Status::kCompleted) << result.message;
  EXPECT_EQ(completion_correlation, "async-model-call");
  server.Join();
}

TEST(ModelsRuntimeTest, AsyncCallDoesNotMintCorrelationWhenCallerHasNone) {
  ASSERT_TRUE(eos::lib::CurrentCorrelationId().empty());
  Runtime runtime;
  std::promise<std::string> completed;
  ASSERT_TRUE(runtime.CallAsync("missing", {}, [&completed](CallResult) {
    completed.set_value(eos::lib::CurrentCorrelationId());
  }));
  EXPECT_TRUE(completed.get_future().get().empty());
}

TEST(ModelsRuntimeTest, AsyncCancellationAbortsActiveRequest) {
  OneShotServer server(R"json({"speech":false,"probability":0.0,"segments":[]})json",
                       http::status::ok, std::chrono::milliseconds(200));
  Runtime runtime({Model("vad", Capability::kVad, Endpoint(server))}, AcceptAll());
  std::atomic<bool> cancel_requested{false};
  std::promise<CallResult> completed;
  auto result = completed.get_future();
  ASSERT_TRUE(runtime.CallAsync(
      "vad", {}, [&completed](CallResult call) { completed.set_value(std::move(call)); },
      [&cancel_requested] { return cancel_requested.load(); }));
  ASSERT_TRUE(server.WaitForRequest());

  cancel_requested = true;
  ASSERT_EQ(result.wait_for(std::chrono::milliseconds(150)), std::future_status::ready);
  EXPECT_EQ(result.get().status, CallResult::Status::kUnavailable);
  server.Join();
}

TEST(ModelsRuntimeTest, ShutdownCancelsActiveRequest) {
  OneShotServer server(R"json({"speech":false,"probability":0.0,"segments":[]})json",
                       http::status::ok, std::chrono::milliseconds(300));
  auto runtime = std::make_unique<Runtime>(
      std::vector<ModelConfig>{Model("vad", Capability::kVad, Endpoint(server))}, AcceptAll());
  std::promise<CallResult> completed;
  auto result = completed.get_future();
  ASSERT_TRUE(runtime->CallAsync(
      "vad", {}, [&completed](CallResult call) { completed.set_value(std::move(call)); }));
  ASSERT_TRUE(server.WaitForRequest());

  const auto started = std::chrono::steady_clock::now();
  runtime.reset();
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(150));
  ASSERT_EQ(result.wait_for(std::chrono::milliseconds(0)), std::future_status::ready);
  EXPECT_EQ(result.get().status, CallResult::Status::kUnavailable);
  server.Join();
}

TEST(ModelsRuntimeTest, ConvertsCanonicalInputToMultipart) {
  OneShotServer server(R"json({"speech":false,"probability":0.0,"segments":[]})json");
  ModelConfig model = Model("vad", Capability::kVad, Endpoint(server, "/v1/vad"));
  model.wire_format = WireFormat::kMultipart;
  Runtime runtime({model}, AcceptAll());
  const nlohmann::json request = {{"input",
                                   {{"data_b64", "AQID"},
                                    {"filename", "chunk.wav"},
                                    {"media_type", "audio/wav"},
                                    {"sample_rate_hz", 16000},
                                    {"channels", 1}}},
                                  {"threshold", 0.4}};

  const CallResult result = runtime.Call("vad", request);
  server.Join();

  EXPECT_EQ(result.status, CallResult::Status::kCompleted);
  EXPECT_NE(server.request()[http::field::content_type].find("multipart/form-data"),
            beast::string_view::npos);
  EXPECT_NE(server.request().body().find("name=\"threshold\""), std::string::npos);
  EXPECT_NE(server.request().body().find("name=\"sample_rate_hz\""), std::string::npos);
  EXPECT_NE(server.request().body().find("name=\"file\"; filename=\"chunk.wav\""),
            std::string::npos);
  EXPECT_NE(server.request().body().find(std::string("\x01\x02\x03", 3)), std::string::npos);
}

TEST(ModelsRuntimeTest, ConvertsDetectionRequestToMultipart) {
  OneShotServer server(
      R"json({"objects":[{"label":"person","class_id":0,"confidence":0.9,"bbox":[10,20,30,40]}],"object_count":1})json");
  ModelConfig model =
      Model("yolo11n-person", Capability::kDetection, Endpoint(server, "/v1/images/detection"));
  model.wire_format = WireFormat::kMultipart;
  Runtime runtime({model}, AcceptAll());
  const nlohmann::json request = {
      {"input", {{"data_b64", "AQID"}, {"filename", "frame.jpg"}, {"media_type", "image/jpeg"}}},
      {"conf", 0.4},
      {"iou", 0.5},
  };

  const CallResult result = runtime.Call("yolo11n-person", request);
  server.Join();

  EXPECT_EQ(result.status, CallResult::Status::kCompleted);
  EXPECT_EQ(server.request().target(), "/v1/images/detection");
  EXPECT_NE(server.request()[http::field::content_type].find("multipart/form-data"),
            beast::string_view::npos);
  EXPECT_NE(server.request().body().find("name=\"conf\""), std::string::npos);
  EXPECT_NE(server.request().body().find("name=\"iou\""), std::string::npos);
  EXPECT_NE(server.request().body().find("name=\"file\"; filename=\"frame.jpg\""),
            std::string::npos);
  EXPECT_EQ(result.response.at("object_count"), 1);
}

TEST(ModelsRuntimeTest, ConvertsAlignedDepthInputToSecondMultipartFile) {
  OneShotServer server(R"json({"persons":[],"person_count":0})json");
  ModelConfig model = Model("pose", Capability::kPose, Endpoint(server, "/v1/images/posture"));
  model.wire_format = WireFormat::kMultipart;
  Runtime runtime({model}, AcceptAll());
  const nlohmann::json request = {
      {"input", {{"data_b64", "AQID"}, {"filename", "frame.jpg"}, {"media_type", "image/jpeg"}}},
      {"depth_input",
       {{"data_b64", "BAUG"}, {"filename", "depth.png"}, {"media_type", "image/png"}}},
      {"depth_scale", 0.001},
  };

  const CallResult result = runtime.Call("pose", request);
  server.Join();

  EXPECT_EQ(result.status, CallResult::Status::kCompleted);
  EXPECT_NE(server.request().body().find("name=\"file\"; filename=\"frame.jpg\""),
            std::string::npos);
  EXPECT_NE(server.request().body().find("name=\"depth_file\"; filename=\"depth.png\""),
            std::string::npos);
  EXPECT_NE(server.request().body().find("name=\"depth_scale\""), std::string::npos);
  EXPECT_NE(server.request().body().find(std::string("\x04\x05\x06", 3)), std::string::npos);
}

TEST(ModelsRuntimeTest, SendsCanonicalArcFaceRecognitionRequest) {
  OneShotServer server(
      R"json({"status":"success","message":"ok","operation":"recognize","data":{"face_count":1,"faces":[{"identity":"Alice","known":true,"similarity":0.81,"bbox":[1,2,30,40],"direction_px":-2.0}]}})json");
  ModelConfig model = Model("arcface", Capability::kFace, Endpoint(server, "/v1/faces"));
  Runtime runtime({model}, AcceptAll());
  const CallResult result = runtime.Call(
      "arcface",
      {{"operation", "recognize"},
       {"input", {{"data_b64", "AQID"}, {"filename", "frame.jpg"}, {"media_type", "image/jpeg"}}}});
  server.Join();

  EXPECT_EQ(result.status, CallResult::Status::kCompleted);
  EXPECT_EQ(server.request().method(), http::verb::post);
  EXPECT_EQ(server.request().target(), "/v1/faces");
  const nlohmann::json request = nlohmann::json::parse(server.request().body());
  EXPECT_EQ(request.at("operation"), "recognize");
  EXPECT_EQ(request.at("input").at("data_b64"), "AQID");
  EXPECT_EQ(result.response.at("operation"), "recognize");
  ASSERT_EQ(result.response.at("data").at("face_count"), 1);
  EXPECT_EQ(result.response.at("data").at("faces").at(0).at("identity"), "Alice");
  EXPECT_TRUE(result.response.at("data").at("faces").at(0).at("known"));
  EXPECT_DOUBLE_EQ(result.response.at("data").at("faces").at(0).at("similarity"), 0.81);
}

TEST(ModelsRuntimeTest, FaceInferenceRejectsRegistrationOperation) {
  OneShotServer server(
      R"json({"status":"success","message":"Registered Alice successfully","operation":"register","data":{"name":"Alice"}})json");
  ModelConfig model = Model("arcface", Capability::kFace, Endpoint(server, "/v1/faces"));
  Runtime runtime({model}, AcceptAll());
  const CallResult result = runtime.Call(
      "arcface",
      {{"operation", "register"},
       {"input", {{"data_b64", "AQID"}, {"filename", "alice.jpg"}, {"media_type", "image/jpeg"}}},
       {"name", "Alice"}});
  server.Join();

  EXPECT_EQ(result.status, CallResult::Status::kProtocolError);
  EXPECT_EQ(server.request().method(), http::verb::post);
  EXPECT_EQ(server.request().target(), "/v1/faces");
  const nlohmann::json request = nlohmann::json::parse(server.request().body());
  EXPECT_EQ(request.at("operation"), "register");
  EXPECT_EQ(request.at("name"), "Alice");
  EXPECT_EQ(result.response.at("operation"), "register");
  EXPECT_EQ(result.response.at("data").at("name"), "Alice");
  EXPECT_EQ(result.response.at("message"), "Registered Alice successfully");
}

TEST(ModelsRuntimeTest, FaceInferenceRejectsListOperation) {
  OneShotServer server(
      R"json({"status":"success","message":"ok","operation":"list","data":{"names":["Alice","Bob"]}})json");
  ModelConfig model = Model("arcface", Capability::kFace, Endpoint(server, "/v1/faces"));
  Runtime runtime({model}, AcceptAll());
  const CallResult result = runtime.Call("arcface", {{"operation", "list"}});
  server.Join();

  EXPECT_EQ(result.status, CallResult::Status::kProtocolError);
  EXPECT_EQ(server.request().method(), http::verb::post);
  EXPECT_EQ(server.request().target(), "/v1/faces");
  EXPECT_EQ(nlohmann::json::parse(server.request().body()),
            nlohmann::json({{"operation", "list"}}));
  EXPECT_EQ(result.response.at("operation"), "list");
  ASSERT_EQ(result.response.at("data").at("names").size(), 2u);
  EXPECT_EQ(result.response.at("data").at("names").at(0), "Alice");
}

TEST(ModelsRuntimeTest, FaceInferenceRejectsDeleteOperation) {
  OneShotServer server(
      R"json({"status":"success","message":"Deleted Alice","operation":"delete","data":{"deleted":"Alice"}})json");
  ModelConfig model = Model("arcface", Capability::kFace, Endpoint(server, "/v1/faces"));
  Runtime runtime({model}, AcceptAll());
  const CallResult result = runtime.Call("arcface", {{"operation", "delete"}, {"name", "Alice"}});
  server.Join();

  EXPECT_EQ(result.status, CallResult::Status::kProtocolError);
  EXPECT_EQ(server.request().method(), http::verb::post);
  EXPECT_EQ(server.request().target(), "/v1/faces");
  const nlohmann::json request = nlohmann::json::parse(server.request().body());
  EXPECT_EQ(request.at("operation"), "delete");
  EXPECT_EQ(request.at("name"), "Alice");
  EXPECT_EQ(result.response.at("operation"), "delete");
  EXPECT_EQ(result.response.at("data").at("deleted"), "Alice");
}

TEST(ModelsRuntimeTest, RejectsMissingOrUnknownFaceOperation) {
  Runtime runtime(
      {Model("arcface", Capability::kFace, "http://127.0.0.1:1/v1/faces")},
      [](const std::string& contract, const nlohmann::json& request) {
        if (contract != "request") return;
        if (!request.contains("operation") || !request.at("operation").is_string() ||
            (request.at("operation") != "recognize" && request.at("operation") != "register" &&
             request.at("operation") != "list" && request.at("operation") != "delete")) {
          throw std::runtime_error("invalid face operation");
        }
      });
  EXPECT_EQ(runtime.Call("arcface", {}).status, CallResult::Status::kInvalidArgument);
  EXPECT_EQ(runtime.Call("arcface", {{"operation", "unknown"}}).status,
            CallResult::Status::kInvalidArgument);
}

TEST(ModelsRuntimeTest, RejectsUnsafeMultipartMetadata) {
  ModelConfig model = Model("vad", Capability::kVad, "http://127.0.0.1:1/vad");
  model.wire_format = WireFormat::kMultipart;
  Runtime runtime({model}, AcceptAll());
  const nlohmann::json request = {
      {"input",
       {{"data_b64", "AQID"}, {"filename", "bad\tname.wav"}, {"media_type", "audio/wav"}}}};
  EXPECT_EQ(runtime.Call("vad", request).status, CallResult::Status::kInvalidArgument);
}

TEST(ModelsRuntimeTest, MapsValidationAndRemoteProtocolFailures) {
  Runtime invalid_request({Model("vad", Capability::kVad, "http://127.0.0.1:1/vad")},
                          [](const std::string& contract, const nlohmann::json&) {
                            if (contract == "request") throw std::runtime_error("bad request");
                          });
  EXPECT_EQ(invalid_request.Call("vad", {}).status, CallResult::Status::kInvalidArgument);

  OneShotServer invalid_json("not-json");
  Runtime protocol({Model("vad", Capability::kVad, Endpoint(invalid_json))}, AcceptAll());
  EXPECT_EQ(protocol.Call("vad", {}).status, CallResult::Status::kProtocolError);
  invalid_json.Join();

  OneShotServer remote_error("busy", http::status::service_unavailable);
  Runtime remote({Model("vad", Capability::kVad, Endpoint(remote_error))}, AcceptAll());
  const CallResult result = remote.Call("vad", {});
  remote_error.Join();
  EXPECT_EQ(result.status, CallResult::Status::kRemoteError);
  EXPECT_EQ(result.http_status, 503u);
  EXPECT_EQ(result.response, "busy");
}

TEST(ModelsRuntimeTest, EnforcesDeadlineAndResponseCount) {
  OneShotServer slow(R"json({"speech":false,"probability":0.0,"segments":[]})json",
                     http::status::ok, std::chrono::milliseconds(200));
  ModelConfig vad = Model("vad", Capability::kVad, Endpoint(slow));
  vad.timeout = std::chrono::milliseconds(20);
  Runtime deadline({vad}, AcceptAll());
  EXPECT_EQ(deadline.Call("vad", {}).status, CallResult::Status::kDeadlineExceeded);
  slow.Join();

  OneShotServer inconsistent(R"json({"persons":[],"person_count":1})json");
  Runtime posture({Model("posture", Capability::kPosture, Endpoint(inconsistent))}, AcceptAll());
  EXPECT_EQ(posture.Call("posture", {}).status, CallResult::Status::kProtocolError);
  inconsistent.Join();
}

TEST(ModelsRuntimeTest, EnforcesVadResponseInvariants) {
  const auto Call = [](const std::string& response) {
    OneShotServer server(response);
    Runtime runtime({Model("vad", Capability::kVad, Endpoint(server))}, AcceptAll());
    const CallResult result = runtime.Call("vad", {});
    server.Join();
    return result;
  };

  EXPECT_EQ(Call(R"json({"speech":true,"probability":0.0,"segments":[]})json").status,
            CallResult::Status::kProtocolError);
  EXPECT_EQ(Call(R"json({"speech":true,"probability":0.8,"segments":[
    {"start_s":0.5,"end_s":0.5,"confidence":0.8}
  ]})json")
                .status,
            CallResult::Status::kProtocolError);
  EXPECT_EQ(Call(R"json({"speech":true,"probability":0.9,"segments":[
    {"start_s":1.0,"end_s":2.0,"confidence":0.9},
    {"start_s":1.5,"end_s":3.0,"confidence":0.8}
  ]})json")
                .status,
            CallResult::Status::kProtocolError);
  EXPECT_EQ(Call(R"json({"speech":true,"probability":0.7,"segments":[
    {"start_s":0.0,"end_s":1.0,"confidence":0.8}
  ]})json")
                .status,
            CallResult::Status::kProtocolError);
  EXPECT_EQ(Call(R"json({"speech":true,"probability":0.9,"segments":[
    {"start_s":0.0,"end_s":1.0,"confidence":0.8},
    {"start_s":1.0,"end_s":2.0,"confidence":0.9}
  ]})json")
                .status,
            CallResult::Status::kCompleted);

  OneShotServer beyond_duration(R"json({"speech":true,"probability":0.8,"segments":[
    {"start_s":0.0,"end_s":0.001,"confidence":0.8}
  ]})json");
  Runtime duration_runtime({Model("vad", Capability::kVad, Endpoint(beyond_duration))},
                           AcceptAll());
  const nlohmann::json short_wav = {
      {"input",
       {{"data_b64", "UklGRiwAAABXQVZFZm10IBAAAAABAAEAgD4AAAB9AAACABAAZGF0YQgAAAAAAAAAAAAAAA=="},
        {"filename", "short.wav"},
        {"media_type", "audio/wav"},
        {"sample_rate_hz", 16000},
        {"channels", 1}}}};
  EXPECT_EQ(duration_runtime.Call("vad", short_wav).status, CallResult::Status::kProtocolError);
  beyond_duration.Join();
}

TEST(ModelsRuntimeTest, RejectsInvertedTemporalAndSpatialRanges) {
  OneShotServer temporal(
      R"json({"text":"hello","duration_s":1.0,"segments":[{"start_s":0.8,"end_s":0.2,"text":"hello"}]})json");
  Runtime asr({Model("asr", Capability::kAsr, Endpoint(temporal))}, AcceptAll());
  EXPECT_EQ(asr.Call("asr", {}).status, CallResult::Status::kProtocolError);
  temporal.Join();

  OneShotServer spatial(
      R"json({"objects":[{"label":"cup","confidence":0.9,"bbox":[20,10,5,30]}],"object_count":1})json");
  Runtime detection({Model("detection", Capability::kDetection, Endpoint(spatial))}, AcceptAll());
  EXPECT_EQ(detection.Call("detection", {}).status, CallResult::Status::kProtocolError);
  spatial.Join();
}

TEST(ModelsRuntimeTest, SupportsQueryWithoutExplicitPath) {
  OneShotServer server(R"json({"speech":false,"probability":0.0,"segments":[]})json");
  const std::string endpoint = "http://127.0.0.1:" + std::to_string(server.port()) + "?language=en";
  Runtime runtime({Model("vad", Capability::kVad, endpoint)}, AcceptAll());
  EXPECT_EQ(runtime.Call("vad", {}).status, CallResult::Status::kCompleted);
  server.Join();
  EXPECT_EQ(server.request().target(), "/?language=en");
}

/// Standard-alphabet base64 decode, enough to inspect a synthesized WAV.
std::string DecodeBase64(const std::string& input) {
  const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  int buffer = 0;
  int bits = 0;
  for (const char character : input) {
    if (character == '=') break;
    const std::size_t index = alphabet.find(character);
    if (index == std::string::npos) continue;
    buffer = (buffer << 6) | static_cast<int>(index);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((buffer >> bits) & 0xff));
    }
  }
  return out;
}

std::uint32_t Le32(const std::string& bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset])) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 2])) << 16) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 3])) << 24);
}

std::string EncodeBase64(const std::string& input) {
  const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  std::size_t index = 0;
  while (index + 2 < input.size()) {
    const unsigned int triple = (static_cast<unsigned char>(input[index]) << 16) |
                                (static_cast<unsigned char>(input[index + 1]) << 8) |
                                static_cast<unsigned char>(input[index + 2]);
    out.push_back(alphabet[(triple >> 18) & 0x3f]);
    out.push_back(alphabet[(triple >> 12) & 0x3f]);
    out.push_back(alphabet[(triple >> 6) & 0x3f]);
    out.push_back(alphabet[triple & 0x3f]);
    index += 3;
  }
  if (index < input.size()) {
    unsigned int triple = static_cast<unsigned char>(input[index]) << 16;
    if (index + 1 < input.size()) {
      triple |= static_cast<unsigned char>(input[index + 1]) << 8;
    }
    out.push_back(alphabet[(triple >> 18) & 0x3f]);
    out.push_back(alphabet[(triple >> 12) & 0x3f]);
    out.push_back(index + 1 < input.size() ? alphabet[(triple >> 6) & 0x3f] : '=');
    out.push_back('=');
  }
  return out;
}

void PushLe16(std::string* out, std::uint16_t value) {
  out->push_back(static_cast<char>(value & 0xff));
  out->push_back(static_cast<char>((value >> 8) & 0xff));
}

void PushLe32(std::string* out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out->push_back(static_cast<char>((value >> shift) & 0xff));
  }
}

/// 16 kHz mono 16-bit WAV of a 440 Hz tone at `amplitude` (0 is silence).
std::string Wav(double seconds, double amplitude) {
  constexpr std::uint32_t kRate = 16000;
  const std::uint32_t frames = static_cast<std::uint32_t>(seconds * kRate);
  std::string pcm;
  for (std::uint32_t frame = 0; frame < frames; ++frame) {
    const double sample = amplitude * std::sin(2.0 * 3.14159265358979323846 * 440.0 *
                                               static_cast<double>(frame) / kRate);
    PushLe16(&pcm, static_cast<std::uint16_t>(static_cast<std::int16_t>(sample * 32767.0)));
  }
  std::string wav;
  wav.append("RIFF", 4);
  PushLe32(&wav, static_cast<std::uint32_t>(36 + pcm.size()));
  wav.append("WAVE", 4);
  wav.append("fmt ", 4);
  PushLe32(&wav, 16);
  PushLe16(&wav, 1);
  PushLe16(&wav, 1);
  PushLe32(&wav, kRate);
  PushLe32(&wav, kRate * 2);
  PushLe16(&wav, 2);
  PushLe16(&wav, 16);
  wav.append("data", 4);
  PushLe32(&wav, static_cast<std::uint32_t>(pcm.size()));
  wav.append(pcm);
  return wav;
}

nlohmann::json AudioRequest(const std::string& wav) {
  return {{"input",
           {{"data_b64", EncodeBase64(wav)},
            {"filename", "clip.wav"},
            {"media_type", "audio/wav"},
            {"sample_rate_hz", 16000},
            {"channels", 1}}}};
}

/// A port nothing is listening on, backed by a socket that is bound but never
/// told to listen. The bound acceptor keeps the port reserved for as long as it
/// lives, so no other test or process can claim it, while connects are still
/// refused exactly as they are for an unstarted model service.
struct ReservedClosedPort {
  asio::io_context io;
  tcp::acceptor acceptor{io};
  std::uint16_t port{0};

  ReservedClosedPort() {
    acceptor.open(tcp::v4());
    acceptor.bind({asio::ip::make_address("127.0.0.1"), 0});
    port = acceptor.local_endpoint().port();
    // Deliberately not calling listen(): this address refuses connections.
  }
};

/// A port reserved for the lifetime of the test binary that never accepts.
std::uint16_t ClosedPort() {
  static const ReservedClosedPort reserved;
  return reserved.port;
}

/// A client whose endpoint is guaranteed to refuse the connection.
ModelConfig Unreachable(std::string id, Capability capability) {
  ModelConfig model = Model(std::move(id), capability,
                            "http://127.0.0.1:" + std::to_string(ClosedPort()) + "/v1/infer");
  model.timeout = std::chrono::milliseconds(150);
  return model;
}

TEST(ModelsRuntimeTest, UnreachableServiceFailsLoudlyWhenFallbackIsOff) {
  Runtime runtime({Unreachable("tts", Capability::kTts)}, AcceptAll());
  const CallResult result = runtime.Call("tts", {{"text", "hello"}});
  EXPECT_EQ(result.status, CallResult::Status::kDeadlineExceeded);
}

TEST(ModelsRuntimeTest, ReconnectsUntilAStartingServiceIsReady) {
  ReservedClosedPort reserved;
  const std::uint16_t port = reserved.port;
  ModelConfig model = Model("detection", Capability::kDetection,
                            "http://127.0.0.1:" + std::to_string(port) + "/v1/infer");
  model.timeout = std::chrono::seconds(2);
  Runtime runtime({model}, AcceptAll());
  std::thread server([&reserved] {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    // The port stayed reserved throughout, so this may start listening without
    // racing another process for the address.
    reserved.acceptor.listen();
    tcp::socket socket(reserved.io);
    reserved.acceptor.accept(socket);
    beast::flat_buffer buffer;
    http::request<http::string_body> request;
    http::read(socket, buffer, request);
    http::response<http::string_body> response{http::status::ok, 11};
    response.set(http::field::content_type, "application/json");
    response.body() = R"json({"detections":[]})json";
    response.prepare_payload();
    http::write(socket, response);
  });

  const CallResult result = runtime.Call("detection", {});
  server.join();
  ASSERT_EQ(result.status, CallResult::Status::kCompleted) << result.message;
  EXPECT_TRUE(result.response.at("detections").empty());
}

}  // namespace
}  // namespace eos::runtime::models

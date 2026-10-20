#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <exception>
#include <functional>
#include <thread>

#include "src/eos/ext/models/external_adapter.h"

namespace eos::ext::models {
namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using runtime::models::CallResult;
using runtime::models::Capability;
using runtime::models::ModelConfig;
using runtime::models::Runtime;
using runtime::models::Transport;

class WebSocketServer final {
 public:
  using Handler = std::function<void(websocket::stream<tcp::socket>&)>;
  explicit WebSocketServer(Handler handler)
      : acceptor_(io_, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)),
        port_(acceptor_.local_endpoint().port()),
        thread_([this, handler = std::move(handler)] {
          try {
            tcp::socket socket(io_);
            acceptor_.accept(socket);
            websocket::stream<tcp::socket> stream(std::move(socket));
            stream.accept();
            handler(stream);
          } catch (...) {
            error_ = std::current_exception();
          }
        }) {}
  ~WebSocketServer() {
    if (thread_.joinable()) thread_.join();
  }
  std::uint16_t port() const { return port_; }
  void Finish() {
    if (thread_.joinable()) thread_.join();
    if (error_) std::rethrow_exception(error_);
  }

 private:
  asio::io_context io_;
  tcp::acceptor acceptor_;
  std::uint16_t port_;
  std::thread thread_;
  std::exception_ptr error_;
};

ModelConfig StreamModel(std::string id, std::uint16_t port, Capability capability) {
  ModelConfig model;
  model.id = std::move(id);
  model.capability = capability;
  model.transport = Transport::kWebSocket;
  model.endpoint =
      "ws://127.0.0.1:" + std::to_string(port) +
      (capability == Capability::kFace ? "/v1/faces/recognize/stream" : "/v1/stream/posture");
  model.adapter = capability == Capability::kFace ? "arcface-stream-v1" : "rtmpose-stream-v1";
  model.timeout = std::chrono::seconds(2);
  return model;
}

nlohmann::json ReadJson(websocket::stream<tcp::socket>* stream) {
  beast::flat_buffer buffer;
  stream->read(buffer);
  return nlohmann::json::parse(beast::buffers_to_string(buffer.data()));
}

void WriteJson(websocket::stream<tcp::socket>* stream, const nlohmann::json& value) {
  stream->text(true);
  stream->write(asio::buffer(value.dump()));
}

Runtime MakeRuntime(ModelConfig model) {
  return Runtime(
      {std::move(model)}, [](const std::string&, const nlohmann::json&) {}, CreateAdapter);
}

TEST(StreamAdapterTest, RtmposeOwnsRequestAndResponseProtocol) {
  WebSocketServer server([](websocket::stream<tcp::socket>& stream) {
    const nlohmann::json request = ReadJson(&stream);
    EXPECT_EQ(request.at("type"), "frame");
    EXPECT_EQ(request.at("image_b64"), "AAAA");
    EXPECT_EQ(request.at("depth_b64"), "BBBB");
    EXPECT_EQ(request.at("furniture").at(0).at("class_name"), "bed");
    WriteJson(&stream, {{"type", "result"},
                        {"frame_index", 1},
                        {"persons",
                         {{{"track_id", 7},
                           {"bbox", {1, 2, 30, 40}},
                           {"confidence", 0.9},
                           {"distance_m", nullptr},
                           {"keypoints", nullptr},
                           {"keypoint_scores", nullptr},
                           {"posture_state", "standing"},
                           {"behavior_event", "none"}}}},
                        {"person_count", 1},
                        {"events", nlohmann::json::array()}});
  });
  Runtime runtime = MakeRuntime(StreamModel("posture", server.port(), Capability::kPosture));
  const CallResult result = runtime.Call("posture", {{"input", {{"data_b64", "AAAA"}}},
                                                      {"depth_input", {{"data_b64", "BBBB"}}},
                                                      {"depth_scale", 0.001},
                                                      {"furniture",
                                                       {{{"bbox", {1, 2, 30, 40}},
                                                         {"class_name", "bed"}}}}});
  ASSERT_EQ(result.status, CallResult::Status::kCompleted) << result.message;
  EXPECT_EQ(result.response.at("person_count"), 1);
  EXPECT_EQ(result.response.at("persons").at(0).at("posture_state"), "standing");
  EXPECT_FALSE(result.response.at("persons").at(0).contains("keypoints"));
  EXPECT_FALSE(result.response.contains("frame_index"));
  server.Finish();
}

TEST(StreamAdapterTest, RtmposeReusesOnePersistentWebSocket) {
  WebSocketServer server([](websocket::stream<tcp::socket>& stream) {
    for (int frame = 0; frame < 2; ++frame) {
      EXPECT_EQ(ReadJson(&stream).at("type"), "frame");
      WriteJson(&stream, {{"type", "result"},
                          {"persons", nlohmann::json::array()},
                          {"person_count", 0}});
    }
  });
  Runtime runtime = MakeRuntime(StreamModel("posture", server.port(), Capability::kPosture));

  EXPECT_EQ(runtime.Call("posture", {{"input", {{"data_b64", "AAAA"}}}}).status,
            CallResult::Status::kCompleted);
  EXPECT_EQ(runtime.Call("posture", {{"input", {{"data_b64", "BBBB"}}}}).status,
            CallResult::Status::kCompleted);
  server.Finish();
}

TEST(StreamAdapterTest, RtmposeRejectsConflictingHumanPoseFieldsAsProtocolError) {
  WebSocketServer server([](websocket::stream<tcp::socket>& stream) {
    ReadJson(&stream);
    WriteJson(&stream, {{"type", "result"},
                        {"persons", {{{"pose", "stand"}, {"posture", "lying"}}}}});
  });
  Runtime runtime = MakeRuntime(StreamModel("human-pose", server.port(), Capability::kHumanPose));

  const CallResult result =
      runtime.Call("human-pose", {{"input", {{"data_b64", "AAAA"}}}});

  EXPECT_EQ(result.status, CallResult::Status::kProtocolError);
  EXPECT_NE(result.message.find("inconsistent fields 'pose' and 'posture'"), std::string::npos);
  server.Finish();
}

TEST(StreamAdapterTest, ArcFaceOwnsRecognitionProtocol) {
  WebSocketServer server([](websocket::stream<tcp::socket>& stream) {
    EXPECT_EQ(ReadJson(&stream).at("max_register_frames"), 1);
    WriteJson(&stream, {{"event", "ready"}});
    beast::flat_buffer image;
    stream.read(image);
    EXPECT_FALSE(stream.got_text());
    WriteJson(&stream, {{"event", "frame_result"},
                        {"faces", {{{"name", "Alice"}, {"score", 0.8}, {"bbox", {1, 2, 3, 4}}}}}});
  });
  Runtime runtime = MakeRuntime(StreamModel("arcface", server.port(), Capability::kFace));
  const CallResult result =
      runtime.Call("arcface", {{"operation", "recognize"}, {"input", {{"data_b64", "/9j/2Q=="}}}});
  ASSERT_EQ(result.status, CallResult::Status::kCompleted) << result.message;
  EXPECT_EQ(result.response.at("data").at("faces").at(0).at("identity"), "Alice");
  server.Finish();
}

TEST(StreamAdapterTest, Yolo11LitterOwnsDetectionProtocolAndPreservesLabels) {
  WebSocketServer server([](websocket::stream<tcp::socket>& stream) {
    const nlohmann::json request = ReadJson(&stream);
    EXPECT_EQ(request.at("type"), "frame");
    EXPECT_EQ(request.at("image_b64"), "AAAA");
    EXPECT_EQ(request.at("conf"), 0.3);
    EXPECT_EQ(request.at("iou"), 0.4);
    WriteJson(&stream,
              {{"type", "result"},
               {"objects",
                 {{{"label", "paper_ball"},
                   {"class_id", 3},
                   {"source_label", "paper"},
                   {"source_class_id", 5},
                  {"confidence", 0.9},
                  {"bbox", {1, 2, 30, 40}}}}},
               {"object_count", 1}});
  });
  ModelConfig model = StreamModel("litter", server.port(), Capability::kDetection);
  model.endpoint = "ws://127.0.0.1:" + std::to_string(server.port()) +
                   "/v1/stream/detections";
  model.adapter = "yolo11-litter-stream-v1";
  Runtime runtime = MakeRuntime(std::move(model));
  const CallResult result = runtime.Call(
      "litter", {{"input", {{"data_b64", "AAAA"}}}, {"conf", 0.3}, {"iou", 0.4}});
  ASSERT_EQ(result.status, CallResult::Status::kCompleted) << result.message;
  EXPECT_EQ(result.response.at("object_count"), 1);
  EXPECT_EQ(result.response.at("objects").at(0).at("label"), "paper_ball");
  EXPECT_EQ(result.response.at("objects").at(0).at("source_label"), "paper");
  EXPECT_EQ(result.response.at("objects").at(0).at("source_class_id"), 5);
  server.Finish();
}

TEST(StreamAdapterTest, Yolo11LitterReconnectsBeforeReusingIdleSocket) {
  asio::io_context io;
  tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
  const auto port = acceptor.local_endpoint().port();
  std::thread provider([&] {
    for (int index = 0; index < 2; ++index) {
      tcp::socket socket(io);
      acceptor.accept(socket);
      websocket::stream<tcp::socket> stream(std::move(socket));
      stream.accept();
      EXPECT_EQ(ReadJson(&stream).at("type"), "frame");
      WriteJson(&stream, {{"type", "result"}, {"objects", nlohmann::json::array()}});
      if (index == 0) {
        beast::error_code ignored;
        stream.next_layer().shutdown(tcp::socket::shutdown_both, ignored);
      }
    }
  });
  ModelConfig model = StreamModel("litter", port, Capability::kDetection);
  model.endpoint = "ws://127.0.0.1:" + std::to_string(port) + "/v1/stream/detections";
  model.adapter = "yolo11-litter-stream-v1";
  model.timeout = std::chrono::milliseconds(200);
  Runtime runtime = MakeRuntime(std::move(model));
  EXPECT_EQ(runtime.Call("litter", {{"input", {{"data_b64", "AAAA"}}}}).status,
            CallResult::Status::kCompleted);
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  EXPECT_EQ(runtime.Call("litter", {{"input", {{"data_b64", "BBBB"}}}}).status,
            CallResult::Status::kCompleted);
  provider.join();
}

TEST(StreamAdapterTest, ArcFaceTranslatesSingleFrameRegistration) {
  WebSocketServer server([](websocket::stream<tcp::socket>& stream) {
    (void)ReadJson(&stream);
    WriteJson(&stream, {{"event", "ready"}});
    const nlohmann::json start = ReadJson(&stream);
    EXPECT_EQ(start.at("event"), "register_start");
    EXPECT_EQ(start.at("frames"), 1);
    WriteJson(&stream, {{"event", "register_started"}, {"name", "Alice"}, {"frames", 1}});
    beast::flat_buffer image;
    stream.read(image);
    WriteJson(&stream, {{"event", "registered"}, {"name", "Alice"}});
  });
  Runtime runtime = MakeRuntime(StreamModel("arcface-register", server.port(), Capability::kFace));
  const CallResult result =
      runtime.RegisterFace("arcface-register", {{"data_b64", "/9j/2Q=="}}, "Alice");
  ASSERT_EQ(result.status, CallResult::Status::kCompleted) << result.message;
  EXPECT_EQ(result.response.at("data").at("name"), "Alice");
  server.Finish();
}

TEST(StreamAdapterTest, CancellationInterruptsProviderRead) {
  WebSocketServer server([](websocket::stream<tcp::socket>& stream) {
    (void)ReadJson(&stream);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  });
  Runtime runtime = MakeRuntime(StreamModel("pose", server.port(), Capability::kPose));
  std::atomic<bool> cancelled{false};
  std::thread cancel([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    cancelled.store(true);
  });
  const CallResult result =
      runtime.Call("pose", {{"input", {{"data_b64", "AAAA"}}}}, [&] { return cancelled.load(); });
  cancel.join();
  EXPECT_EQ(result.status, CallResult::Status::kUnavailable);
  server.Finish();
}

}  // namespace
}  // namespace eos::ext::models

#include "src/eos/ext/backends/builtin.h"
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <future>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>

#include "ack.h"
#include "common.h"
#include "speaker_audio.h"
#include "src/eos/ext/simulator/ipc/client.h"
#include "src/eos/runtime/actionlist/manager.h"
#include "src/eos/runtime/models/runtime.h"

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

class BlockingTtsServer {
 public:
  BlockingTtsServer()
      : acceptor_(io_, {asio::ip::make_address("127.0.0.1"), 0}), worker_([this] { Serve(); }) {}

  ~BlockingTtsServer() {
    ReleaseSynthesis();
    beast::error_code ignored;
    acceptor_.close(ignored);
    if (worker_.joinable()) worker_.join();
  }

  std::string Endpoint(const char* target) const {
    return "http://127.0.0.1:" + std::to_string(acceptor_.local_endpoint().port()) + target;
  }

  bool WaitForSynthesis() {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, std::chrono::seconds(2), [this] { return synthesis_received_; });
  }

  void ReleaseSynthesis() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      synthesis_released_ = true;
    }
    changed_.notify_all();
  }

 private:
  void Serve() {
    for (int request_index = 0; request_index < 2; ++request_index) {
      tcp::socket socket(io_);
      beast::error_code error;
      acceptor_.accept(socket, error);
      if (error) return;
      beast::flat_buffer buffer;
      http::request<http::string_body> request;
      http::read(socket, buffer, request);
      std::string body;
      if (request.target() == "/health") {
        body =
            R"json({"interface_version":"1.0","status":"ready","service":{"name":"tts","version":"1"},"model":{"id":"audio8","version":"1"},"capabilities":["audio.synthesis"]})json";
      } else {
        {
          std::lock_guard<std::mutex> lock(mutex_);
          synthesis_received_ = true;
        }
        changed_.notify_all();
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [this] { return synthesis_released_; });
        body = R"json({})json";
      }
      http::response<http::string_body> response{http::status::ok, 11};
      response.set(http::field::content_type, "application/json");
      response.body() = std::move(body);
      response.prepare_payload();
      beast::error_code ignored;
      http::write(socket, response, ignored);
      socket.shutdown(tcp::socket::shutdown_both, ignored);
    }
  }

  asio::io_context io_;
  tcp::acceptor acceptor_;
  std::mutex mutex_;
  std::condition_variable changed_;
  bool synthesis_received_{false};
  bool synthesis_released_{false};
  std::thread worker_;
};

std::vector<std::uint8_t> Wave() {
  return {'R', 'I', 'F', 'F', 40,  0,   0,   0,   'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
          16,  0,   0,   0,   1,   0,   1,   0,   128, 62,  0,   0,   0,   125, 0,   0,
          2,   0,   16,  0,   'd', 'a', 't', 'a', 4,   0,   0,   0,   0,   0,   1,   0};
}

std::vector<std::uint8_t> PcmWave(std::uint32_t sample_rate, std::uint16_t channels,
                                  std::size_t frames) {
  std::vector<std::uint8_t> bytes(44 + frames * channels * 2);
  const auto write16 = [&](std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
  };
  const auto write32 = [&](std::size_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 3] = static_cast<std::uint8_t>(value >> 24);
  };
  std::copy_n("RIFF", 4, bytes.begin());
  write32(4, static_cast<std::uint32_t>(bytes.size() - 8));
  std::copy_n("WAVEfmt ", 8, bytes.begin() + 8);
  write32(16, 16);
  write16(20, 1);
  write16(22, channels);
  write32(24, sample_rate);
  write32(28, sample_rate * channels * 2);
  write16(32, channels * 2);
  write16(34, 16);
  std::copy_n("data", 4, bytes.begin() + 36);
  write32(40, static_cast<std::uint32_t>(frames * channels * 2));
  for (std::size_t frame = 0; frame < frames; ++frame) {
    for (std::size_t channel = 0; channel < channels; ++channel) {
      write16(44 + (frame * channels + channel) * 2, static_cast<std::uint16_t>(frame % 200));
    }
  }
  return bytes;
}

class AssetDirectory {
 public:
  AssetDirectory() {
    root_ = std::filesystem::temp_directory_path() /
            ("eos-dispatch-assets-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root_ / "alerts");
    const std::vector<std::uint8_t> wave = Wave();
    std::ofstream output(root_ / "alerts" / "ready.wav", std::ios::binary);
    output.write(reinterpret_cast<const char*>(wave.data()),
                 static_cast<std::streamsize>(wave.size()));
  }

  ~AssetDirectory() { std::filesystem::remove_all(root_); }

  const std::filesystem::path& root() const { return root_; }

 private:
  std::filesystem::path root_;
};

bool WaitForBaseVelocity(eos::runtime::hardware_runtime::Registry& registry) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (std::chrono::steady_clock::now() < deadline) {
    if (dispatch_test::BaseState(registry).linear_x_m_s != 0.0) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

}  // namespace

using namespace dispatch_test;

TEST(SpeakerAudioTest, NormalizesPcm16WaveTo16KhzMono) {
  for (const std::uint32_t sample_rate : {24000, 44100}) {
    const auto input = PcmWave(sample_rate, 2, sample_rate / 100);
    ASSERT_FALSE(eos::runtime::dispatch::detail::IsPcmWave(input));

    const auto normalized = eos::runtime::dispatch::detail::NormalizePcmWave(input);
    ASSERT_TRUE(normalized.has_value());
    EXPECT_TRUE(eos::runtime::dispatch::detail::IsPcmWave(*normalized));
    EXPECT_EQ(normalized->size(), 44 + 160 * 2);
  }
}

TEST(SpeakerAudioTest, PreservesCanonicalWave) {
  const auto input = PcmWave(16000, 1, 160);
  const auto normalized = eos::runtime::dispatch::detail::NormalizePcmWave(input);
  ASSERT_TRUE(normalized.has_value());
  EXPECT_EQ(*normalized, input);
}

TEST(SpeakerAudioTest, RejectsNonPcmWave) {
  auto input = PcmWave(24000, 1, 240);
  input[20] = 3;
  EXPECT_FALSE(eos::runtime::dispatch::detail::NormalizePcmWave(input).has_value());
}

TEST(PrimitiveDispatchTest, ReleasesBackendAlignedDevicePrimitives) {
  auto registry = DemoRobot();
  AssetDirectory assets;
  eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store(), false, nullptr,
                                                assets.root());
  ExpectAccepted(Exec(dispatcher, "base.posture", "base-0", {{"posture", "walk"}}), "base.posture");
  ExpectAccepted(Exec(dispatcher, "base.navigate", "base-0",
                      {{"reference", "relative"}, {"x_m", 1.0}, {"y_m", -0.5}, {"yaw_rad", 0.25}}),
                 "base.navigate");
  const auto base_state = BaseState(registry);
  EXPECT_DOUBLE_EQ(base_state.x_m, 1.0);
  EXPECT_DOUBLE_EQ(base_state.y_m, -0.5);
  EXPECT_DOUBLE_EQ(base_state.yaw_rad, 0.25);
  ExpectAccepted(Exec(dispatcher, "gimbal.motion", "gimbal-0",
                      {{"reference", "absolute"}, {"pan_rad", 0.5}, {"tilt_rad", 0.2}}),
                 "gimbal.motion");
  ExpectAccepted(
      Exec(dispatcher, "arm.movej", "arm-0",
           {{"reference", "absolute"}, {"joint_positions_rad", {0.5, 0.6, 0.5, 0.5, 0.5, 0.5}}}),
      "arm.movej");
  ExpectAccepted(Exec(dispatcher, "arm.movel", "arm-0",
                      {{"reference", "absolute"},
                       {"frame_id", "base_link"},
                       {"position_m", {0.2, 0.1, 0.3}},
                       {"orientation_quat_xyzw", {0.0, 0.0, 0.0, 1.0}}}),
                 "arm.movel");
  ExpectAccepted(Exec(dispatcher, "gripper.motion", "gripper-0", {{"action", "close"}}),
                 "gripper.motion");
  ExpectAccepted(
      Exec(dispatcher, "speaker.play", "speaker-0", {{"uri", "asset://alerts/ready.wav"}}),
      "speaker.play");
}

TEST(PrimitiveDispatchTest, PositionPrimitivesAdmitDefaultAndStreamingModes) {
  auto registry = DemoRobot();
  eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store());

  ExpectAccepted(Exec(dispatcher, "base.navigate", "base-0",
                      {{"reference", "relative"},
                       {"x_m", 0.1},
                       {"y_m", 0.0},
                       {"yaw_rad", 0.0},
                       {"stream", true}}),
                 "base.navigate");
  ExpectAccepted(Exec(dispatcher, "base.navigate", "base-0",
                      {{"reference", "absolute"},
                       {"frame_id", "map"},
                       {"x_m", 0.2},
                       {"y_m", 0.1},
                       {"yaw_rad", 0.1},
                       {"stream", true}}),
                 "base.navigate");
  ExpectAccepted(
      Exec(dispatcher, "gimbal.motion", "gimbal-0",
           {{"reference", "absolute"}, {"pan_rad", 0.1}, {"tilt_rad", 0.2}, {"stream", true}}),
      "gimbal.motion");
  ExpectAccepted(Exec(dispatcher, "arm.movej", "arm-0",
                      {{"reference", "absolute"},
                       {"joint_positions_rad", {0.1, 0.1, 0.1, 0.1, 0.1, 0.1}},
                       {"stream", true}}),
                 "arm.movej");
  ExpectAccepted(Exec(dispatcher, "arm.movel", "arm-0",
                      {{"reference", "absolute"},
                       {"frame_id", "base_link"},
                       {"position_m", {0.2, 0.1, 0.3}},
                       {"orientation_quat_xyzw", {0.0, 0.0, 0.0, 1.0}},
                       {"stream", true}}),
                 "arm.movel");

  ExpectInvalid(dispatcher, "arm.movej", "arm-0",
                {{"reference", "absolute"},
                 {"joint_positions_rad", {0.1, 0.1, 0.1, 0.1, 0.1, 0.1}},
                 {"stream", "true"}});
}

TEST(PrimitiveDispatchTest, ResolvesRelativeMotionFromCurrentState) {
  auto registry = DemoRobot();
  eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store());
  ExpectAccepted(
      Exec(dispatcher, "arm.movej", "arm-0",
           {{"reference", "absolute"}, {"joint_positions_rad", {0.1, 0.1, 0.1, 0.1, 0.1, 0.1}}}),
      "arm.movej");
  ExpectAccepted(
      Exec(dispatcher, "arm.movej", "arm-0",
           {{"reference", "relative"}, {"joint_positions_rad", {0.2, 0.2, 0.2, 0.2, 0.2, 0.2}}}),
      "arm.movej");
  for (double joint : ArmState(registry).joint_angles_rad) {
    EXPECT_NEAR(joint, 0.3, 1e-12);
  }

  ExpectAccepted(Exec(dispatcher, "gimbal.motion", "gimbal-0",
                      {{"reference", "absolute"}, {"pan_rad", 0.4}, {"tilt_rad", 0.2}}),
                 "gimbal.motion");
  ExpectAccepted(Exec(dispatcher, "gimbal.motion", "gimbal-0",
                      {{"reference", "relative"}, {"pan_rad", 0.1}, {"tilt_rad", -0.1}}),
                 "gimbal.motion");
  const auto gimbal = GimbalState(registry);
  EXPECT_NEAR(gimbal.pan_rad, 0.5, 1e-12);
  EXPECT_NEAR(gimbal.tilt_rad, 0.1, 1e-12);
}

TEST(PrimitiveDispatchTest, AccumulatesStreamedRelativeGimbalTargetsUntilStop) {
  auto registry = DemoRobot();
  eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store());
  eos::ext::simulator::ipc::SimulatorClient simulator("var/run/simulator.sock");
  ASSERT_TRUE(simulator.PointGimbal("gimbal-0", 0.0, 0.0, 0.0, 0.0));

  const auto first = dispatcher.ExecuteSkillStep("gimbal.motion", "1.0.0", "gimbal-0",
                                                 {{"reference", "relative"},
                                                  {"pan_rad", 0.1},
                                                  {"tilt_rad", -0.05},
                                                  {"stream", true},
                                                  {"command_id", "stream-relative-1"}});
  ASSERT_EQ(first.status, eos::runtime::dispatch::DispatchResult::Status::kCompleted);
  ASSERT_TRUE(first.payload.at("accepted").get<bool>());

  // Hold physical telemetry at its old value to model the 10 Hz feedback path.
  ASSERT_TRUE(simulator.PointGimbal("gimbal-0", 0.0, 0.0, 0.0, 0.0));
  const auto second = dispatcher.ExecuteSkillStep("gimbal.motion", "1.0.0", "gimbal-0",
                                                  {{"reference", "relative"},
                                                   {"pan_rad", 0.2},
                                                   {"tilt_rad", -0.1},
                                                   {"stream", true},
                                                   {"command_id", "stream-relative-2"}});
  ASSERT_EQ(second.status, eos::runtime::dispatch::DispatchResult::Status::kCompleted);
  ASSERT_TRUE(second.payload.at("accepted").get<bool>());
  auto gimbal = GimbalState(registry);
  EXPECT_NEAR(gimbal.pan_rad, 0.3, 1e-12);
  EXPECT_NEAR(gimbal.tilt_rad, -0.15, 1e-12);

  ExpectAccepted(Exec(dispatcher, "motion.stop", "gimbal-0"), "motion.stop");
  ASSERT_TRUE(simulator.PointGimbal("gimbal-0", 0.0, 0.0, 0.0, 0.0));
  const auto after_stop =
      dispatcher.ExecuteSkillStep("gimbal.motion", "1.0.0", "gimbal-0",
                                  {{"reference", "relative"},
                                   {"pan_rad", 0.2},
                                   {"tilt_rad", -0.1},
                                   {"stream", true},
                                   {"command_id", "stream-relative-after-stop"}});
  ASSERT_EQ(after_stop.status, eos::runtime::dispatch::DispatchResult::Status::kCompleted);
  ASSERT_TRUE(after_stop.payload.at("accepted").get<bool>());
  gimbal = GimbalState(registry);
  EXPECT_NEAR(gimbal.pan_rad, 0.2, 1e-12);
  EXPECT_NEAR(gimbal.tilt_rad, -0.1, 1e-12);
}

TEST(PrimitiveDispatchTest, SkillStepsReturnTerminalResultsAndRejectUnknownBoundaries) {
  auto registry = DemoRobot();
  eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store());

  const auto gimbal = dispatcher.ExecuteSkillStep("gimbal.motion", "1.0.0", "gimbal-0",
                                                  {{"reference", "absolute"},
                                                   {"pan_rad", 0.4},
                                                   {"tilt_rad", 0.2},
                                                   {"command_id", "skill-gimbal"}});
  ASSERT_EQ(gimbal.status, eos::runtime::dispatch::DispatchResult::Status::kCompleted);
  EXPECT_TRUE(gimbal.payload.at("accepted").get<bool>());
  EXPECT_EQ(gimbal.payload.at("result").at("status"), "succeeded");

  int cancellation_checks = 0;
  const auto streamed = dispatcher.ExecuteSkillStep("gimbal.motion", "1.0.0", "gimbal-0",
                                                    {{"reference", "absolute"},
                                                     {"pan_rad", 0.5},
                                                     {"tilt_rad", 0.1},
                                                     {"stream", true},
                                                     {"command_id", "skill-gimbal-stream"}},
                                                    [&] { return ++cancellation_checks > 1; });
  ASSERT_EQ(streamed.status, eos::runtime::dispatch::DispatchResult::Status::kCompleted);
  EXPECT_TRUE(streamed.payload.at("accepted").get<bool>());
  EXPECT_EQ(streamed.payload.at("result").at("status"), "succeeded");
  EXPECT_EQ(cancellation_checks, 1);

  const auto initialized =
      dispatcher.ExecuteSkillStep("arm.init", "1.0.0", "arm-0", {{"command_id", "skill-arm-init"}});
  ASSERT_EQ(initialized.status, eos::runtime::dispatch::DispatchResult::Status::kCompleted);
  EXPECT_TRUE(initialized.payload.at("accepted").get<bool>());
  EXPECT_EQ(initialized.payload.at("result").at("status"), "succeeded");

  const auto before_state = ArmState(registry);
  const std::vector<double> before(std::begin(before_state.joint_angles_rad),
                                   std::end(before_state.joint_angles_rad));
  const auto arm =
      dispatcher.ExecuteSkillStep("arm.movej", "2.0.0", "arm-0",
                                  {{"reference", "absolute"},
                                   {"joint_positions_rad", {0.5, 0.5, 0.5, 0.5, 0.5, 0.5}},
                                   {"command_id", "skill-arm-old-version"}});
  ASSERT_EQ(arm.status, eos::runtime::dispatch::DispatchResult::Status::kNotFound);
  const auto after_state = ArmState(registry);
  EXPECT_TRUE(
      std::equal(before.begin(), before.end(), std::begin(after_state.joint_angles_rad)));
}

TEST(PrimitiveDispatchTest, CommandIdsReplayExactlyOnceAndRejectConflicts) {
  auto registry = DemoRobot();
  eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store());
  const nlohmann::json arguments{{"posture", "lie"}};
  const auto first =
      dispatcher.ExecuteCommand("base.posture", "1.0.0", "base-0", arguments, "posture-command-1");
  ASSERT_EQ(first.status, eos::runtime::dispatch::DispatchResult::Status::kCompleted);
  EXPECT_EQ(BaseState(registry).posture, eos::hardware::BasePosture::kLie);

  const auto replay =
      dispatcher.ExecuteCommand("base.posture", "1.0.0", "base-0", arguments, "posture-command-1");
  EXPECT_EQ(replay.payload, first.payload);

  const auto conflict = dispatcher.ExecuteCommand("base.posture", "1.0.0", "base-0",
                                                  {{"posture", "walk"}}, "posture-command-1");
  EXPECT_EQ(conflict.status, eos::runtime::dispatch::DispatchResult::Status::kAlreadyExists);
  EXPECT_EQ(BaseState(registry).posture, eos::hardware::BasePosture::kLie);
}

TEST(PrimitiveDispatchTest, SkillAndDirectCommandsShareFifoDeviceAdmission) {
  auto registry = DemoRobotWithBaseCapabilities(true);
  eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store());
  auto velocity = std::async(std::launch::async, [&] {
    return dispatcher.ExecuteSkillStep("base.velocity", "1.0.0", "base-0",
                                       {{"linear_x_m_s", 0.1},
                                        {"linear_y_m_s", 0.0},
                                        {"angular_z_rad_s", 0.0},
                                        {"valid_for_ms", 120},
                                        {"command_id", "skill-velocity"}});
  });
  ASSERT_TRUE(WaitForBaseVelocity(registry));

  const nlohmann::json direct_velocity{{"linear_x_m_s", -0.1},
                                       {"linear_y_m_s", 0.0},
                                       {"angular_z_rad_s", 0.0},
                                       {"valid_for_ms", 100}};
  auto direct = std::async(std::launch::async, [&] {
    return dispatcher.ExecuteCommand("base.velocity", "1.0.0", "base-0", direct_velocity,
                                     "direct-velocity");
  });
  EXPECT_EQ(direct.wait_for(std::chrono::milliseconds(30)), std::future_status::timeout);
  EXPECT_EQ(velocity.get().payload.at("result").at("status"), "succeeded");
  const auto direct_result = direct.get();
  ASSERT_EQ(direct_result.status, eos::runtime::dispatch::DispatchResult::Status::kCompleted);
  EXPECT_TRUE(direct_result.payload.at("accepted").get<bool>()) << direct_result.payload.dump();
  EXPECT_EQ(direct_result.payload.at("result").at("status"), "succeeded")
      << direct_result.payload.dump();
  EXPECT_EQ(
      dispatcher
          .ExecuteCommand("base.velocity", "1.0.0", "base-0", direct_velocity, "direct-velocity")
          .payload,
      direct_result.payload);
}

TEST(PrimitiveDispatchTest, QueuedCommandCancellationNeverReleasesTheBackend) {
  auto registry = DemoRobotWithBaseCapabilities(true);
  eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store());
  auto active = std::async(std::launch::async, [&] {
    return dispatcher.ExecuteCommand("base.velocity", "1.0.0", "base-0",
                                     {{"linear_x_m_s", 0.1},
                                      {"linear_y_m_s", 0.0},
                                      {"angular_z_rad_s", 0.0},
                                      {"valid_for_ms", 1000}},
                                     "active-before-queued-cancel");
  });
  ASSERT_TRUE(WaitForBaseVelocity(registry));
  const auto before = BaseState(registry);
  auto queued = std::async(std::launch::async, [&] {
    return dispatcher.ExecuteCommand(
        "base.navigate", "1.0.0", "base-0",
        {{"reference", "relative"}, {"x_m", 2.0}, {"y_m", 0.0}, {"yaw_rad", 0.0}}, "queued-cancel");
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  const auto cancellation = dispatcher.CancelCommand("queued-cancel");
  EXPECT_EQ(cancellation.payload.at("result").at("status"), "cancelled");
  EXPECT_EQ(queued.get().payload, cancellation.payload);
  EXPECT_LT(std::abs(BaseState(registry).x_m - before.x_m), 0.2);
  dispatcher.CancelCommand("active-before-queued-cancel");
  EXPECT_EQ(active.get().payload.at("result").at("status"), "cancelled");
}

TEST(PrimitiveDispatchTest, NavigationCancellationRequiresActiveMatchingCommand) {
  using eos::runtime::dispatch::NavigationCancelStatus;
  auto registry = DemoRobotWithBaseCapabilities(true);
  eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store());
  EXPECT_EQ(dispatcher.CancelActiveNavigationCommand("missing", "base-0"),
            NavigationCancelStatus::kNotFound);
  auto active = std::async(std::launch::async, [&] {
    return dispatcher.ExecuteCommand("base.velocity", "1.0.0", "base-0",
                                     {{"linear_x_m_s", 0.1},
                                      {"linear_y_m_s", 0.0},
                                      {"angular_z_rad_s", 0.0},
                                      {"valid_for_ms", 1000}},
                                     "active-before-targeted-cancel");
  });
  ASSERT_TRUE(WaitForBaseVelocity(registry));
  EXPECT_EQ(dispatcher.CancelActiveNavigationCommand("active-before-targeted-cancel", "base-0"),
            NavigationCancelStatus::kMismatch);
  auto queued = std::async(std::launch::async, [&] {
    return dispatcher.ExecuteCommand(
        "base.navigate", "1.0.0", "base-0",
        {{"reference", "relative"}, {"x_m", 2.0}, {"y_m", 0.0}, {"yaw_rad", 0.0}},
        "targeted-queued-navigation");
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  EXPECT_EQ(dispatcher.CancelActiveNavigationCommand("targeted-queued-navigation", "other-base"),
            NavigationCancelStatus::kMismatch);
  EXPECT_EQ(dispatcher.CancelActiveNavigationCommand("targeted-queued-navigation", "base-0"),
            NavigationCancelStatus::kCancelled);
  EXPECT_EQ(queued.get().payload.at("result").at("status"), "cancelled");
  EXPECT_EQ(dispatcher.CancelActiveNavigationCommand("targeted-queued-navigation", "base-0"),
            NavigationCancelStatus::kNotFound);
  dispatcher.CancelCommand("active-before-targeted-cancel");
  EXPECT_EQ(active.get().payload.at("result").at("status"), "cancelled");
}

TEST(PrimitiveDispatchTest, DeviceAdmissionWaitHasABoundedReplayableDeadline) {
  auto registry = DemoRobotWithBaseCapabilities(true);
  eos::runtime::dispatch::AdmissionOptions admission;
  admission.max_commands_per_device = 4;
  admission.max_wait = std::chrono::milliseconds(50);
  eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store(), false, nullptr,
                                                {}, {}, admission);
  auto active = std::async(std::launch::async, [&] {
    return dispatcher.ExecuteCommand("base.velocity", "1.0.0", "base-0",
                                     {{"linear_x_m_s", 0.1},
                                      {"linear_y_m_s", 0.0},
                                      {"angular_z_rad_s", 0.0},
                                      {"valid_for_ms", 1000}},
                                     "active-before-deadline");
  });
  ASSERT_TRUE(WaitForBaseVelocity(registry));
  const nlohmann::json posture{{"posture", "lie"}};
  const auto timed_out =
      dispatcher.ExecuteCommand("base.posture", "1.0.0", "base-0", posture, "admission-deadline");
  ASSERT_EQ(timed_out.status, eos::runtime::dispatch::DispatchResult::Status::kCompleted);
  EXPECT_EQ(timed_out.payload.at("result").at("status"), "timed_out");
  EXPECT_FALSE(timed_out.payload.at("accepted").get<bool>());
  EXPECT_EQ(
      dispatcher.ExecuteCommand("base.posture", "1.0.0", "base-0", posture, "admission-deadline")
          .payload,
      timed_out.payload);
  dispatcher.CancelCommand("active-before-deadline");
  active.get();
}

TEST(PrimitiveDispatchTest, DeviceAdmissionCapacityIsBoundedAndReplayable) {
  auto registry = DemoRobotWithBaseCapabilities(true);
  eos::runtime::dispatch::AdmissionOptions admission;
  admission.max_commands_per_device = 1;
  eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store(), false, nullptr,
                                                {}, {}, admission);
  auto active = std::async(std::launch::async, [&] {
    return dispatcher.ExecuteCommand("base.velocity", "1.0.0", "base-0",
                                     {{"linear_x_m_s", 0.1},
                                      {"linear_y_m_s", 0.0},
                                      {"angular_z_rad_s", 0.0},
                                      {"valid_for_ms", 1000}},
                                     "capacity-active");
  });
  ASSERT_TRUE(WaitForBaseVelocity(registry));
  const nlohmann::json posture{{"posture", "lie"}};
  const auto rejected =
      dispatcher.ExecuteCommand("base.posture", "1.0.0", "base-0", posture, "capacity-rejected");
  ASSERT_EQ(rejected.status, eos::runtime::dispatch::DispatchResult::Status::kCompleted);
  EXPECT_FALSE(rejected.payload.at("accepted").get<bool>());
  EXPECT_EQ(rejected.payload.at("reason_code"), "rejected.busy");
  EXPECT_EQ(rejected.payload.at("result").at("status"), "rejected");
  EXPECT_EQ(
      dispatcher.ExecuteCommand("base.posture", "1.0.0", "base-0", posture, "capacity-rejected")
          .payload,
      rejected.payload);
  dispatcher.CancelCommand("capacity-active");
  active.get();
}

TEST(PrimitiveDispatchTest, SameDeviceActionListRootsCompleteThroughFifoAdmission) {
  auto registry = DemoRobotWithBaseCapabilities(true);
  eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store());
  eos::runtime::actionlist::SkillRunner skills;
  skills.start = [](const std::string&, const std::string&, nlohmann::json,
                    const std::string&) -> eos::runtime::skills::StartResult {
    throw std::runtime_error("unexpected skill action");
  };
  skills.cancel = [](const std::string&) { return true; };
  skills.list_executions = [] { return std::vector<eos::runtime::skills::ExecutionSnapshot>{}; };
  eos::runtime::actionlist::PrimitiveRunner primitives;
  primitives.execute = [&](const std::string& name, const std::string& version,
                           const std::string& device_id, nlohmann::json arguments,
                           const std::string& command_id, const std::function<bool()>& cancelled) {
    auto result = dispatcher.ExecuteCommand(name, version, device_id, std::move(arguments),
                                            command_id, cancelled);
    if (result.status != eos::runtime::dispatch::DispatchResult::Status::kCompleted)
      throw std::runtime_error(result.message);
    return result.payload;
  };
  primitives.cancel = [&](const std::string& command_id) {
    (void)dispatcher.CancelCommand(command_id);
  };
  eos::runtime::actionlist::Manager action_lists(std::move(skills), std::move(primitives), {2});
  const nlohmann::json velocity{
      {"linear_x_m_s", 0.1}, {"linear_y_m_s", 0.0}, {"angular_z_rad_s", 0.0}, {"valid_for_ms", 80}};
  action_lists.Submit(
      {{"workflow_id", "same-device-roots"},
       {"actions",
        {{{"action_id", "first"},
          {"primitive", {{"name", "base.velocity"}, {"version", "1.0.0"}, {"device_id", "base-0"}}},
          {"parameters", velocity}},
         {{"action_id", "second"},
          {"primitive", {{"name", "base.velocity"}, {"version", "1.0.0"}, {"device_id", "base-0"}}},
          {"parameters", velocity}}}}});
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  std::optional<nlohmann::json> terminal;
  while (std::chrono::steady_clock::now() < deadline) {
    terminal = action_lists.Get("same-device-roots");
    if (terminal && terminal->value("status", "") == "succeeded") break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_TRUE(terminal.has_value());
  ASSERT_EQ(terminal->at("status"), "succeeded") << terminal->dump();
  for (const auto& action : terminal->at("actions")) {
    EXPECT_EQ(action.at("status"), "succeeded");
    EXPECT_TRUE(action.at("result").at("accepted").get<bool>());
    EXPECT_TRUE(action.at("result").at("reason_code").is_null());
  }
}

TEST(PrimitiveDispatchTest, SpeakerCommandReturnsAfterPlaybackCompletes) {
  AssetDirectory assets;
  auto registry = DemoRobot();
  eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store(), false, nullptr,
                                                assets.root());
  auto playback = std::async(std::launch::async, [&] {
    return dispatcher.ExecuteCommand("speaker.play", "1.0.0", "speaker-0",
                                     {{"uri", "asset://alerts/ready.wav"}}, "speaker-terminal");
  });
  for (int attempt = 0; attempt < 500 && playback.wait_for(std::chrono::milliseconds(0)) !=
                                             std::future_status::ready;
       ++attempt) {
    dispatcher.TickHardware(0.05);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_EQ(playback.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  const auto result = playback.get();
  ASSERT_EQ(result.status, eos::runtime::dispatch::DispatchResult::Status::kCompleted);
  EXPECT_EQ(result.payload.at("result").at("status"), "succeeded");
  const auto speaker =
      registry.Find(eos::hardware::DeviceKind::kSpeaker).feeds().ReadSpeakerTelemetry();
  ASSERT_TRUE(speaker.has_value());
  EXPECT_EQ(speaker->state, eos::hardware::SpeakerPlaybackState::kIdle);
}

TEST(PrimitiveDispatchTest, DurableLedgerReplaysAndNeverReleasesPreparedCommands) {
  AssetDirectory state;
  const std::filesystem::path journal = state.root() / "commands.jsonl";
  nlohmann::json first_response;
  {
    auto registry = DemoRobot();
    eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store(), false, nullptr,
                                                  {}, journal);
    const auto first = dispatcher.ExecuteCommand("base.posture", "1.0.0", "base-0",
                                                 {{"posture", "lie"}}, "durable-command-1");
    ASSERT_EQ(first.status, eos::runtime::dispatch::DispatchResult::Status::kCompleted);
    first_response = first.payload;
  }
  {
    auto registry = DemoRobot();
    eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store(), false, nullptr,
                                                  {}, journal);
    const auto replay = dispatcher.ExecuteCommand("base.posture", "1.0.0", "base-0",
                                                  {{"posture", "lie"}}, "durable-command-1");
    EXPECT_EQ(replay.payload, first_response);
    EXPECT_EQ(BaseState(registry).posture, eos::hardware::BasePosture::kLie);
  }

  const std::filesystem::path prepared_journal = state.root() / "prepared.jsonl";
  nlohmann::json canonical_arguments{{"posture", "lie"}, {"type", "base.posture"}};
  const std::string fingerprint = nlohmann::json{
      {"primitive", "base.posture"},
      {"version", "1.0.0"},
      {"device_id", "base-0"},
      {"arguments",
       canonical_arguments}}.dump();
  std::ofstream(prepared_journal) << nlohmann::json{{"type", "prepared"},
                                                    {"command_id", "prepared-command-1"},
                                                    {"fingerprint", fingerprint},
                                                    {"primitive", "base.posture"},
                                                    {"version", "1.0.0"},
                                                    {"device_id", "base-0"},
                                                    {"cancellable", true}}
                                         .dump()
                                  << '\n';
  auto registry = DemoRobot();
  eos::runtime::dispatch::Dispatcher recovered(registry, dispatch_test::Store(), false, nullptr, {},
                                               prepared_journal);
  const auto unknown = recovered.ExecuteCommand("base.posture", "1.0.0", "base-0",
                                                {{"posture", "lie"}}, "prepared-command-1");
  ASSERT_EQ(unknown.status, eos::runtime::dispatch::DispatchResult::Status::kCompleted);
  EXPECT_EQ(unknown.payload.at("result").at("status"), "outcome_unknown");
  EXPECT_EQ(BaseState(registry).posture, eos::hardware::BasePosture::kLie);
}

TEST(PrimitiveDispatchTest, DurableLedgerReloadsTransportErrorsWithoutPayloads) {
  AssetDirectory state;
  const std::filesystem::path journal = state.root() / "commands.jsonl";
  eos::runtime::dispatch::DispatchResult invalid;
  {
    auto registry = DemoRobot();
    eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store(), false, nullptr,
                                                  {}, journal);
    invalid = dispatcher.ExecuteCommand("gimbal.motion", "1.0.0", "gimbal-0",
                                        nlohmann::json::object(), "invalid-command-1");
    ASSERT_EQ(invalid.status, eos::runtime::dispatch::DispatchResult::Status::kInvalidArgument);
    EXPECT_TRUE(invalid.payload.is_null());
  }
  {
    auto registry = DemoRobot();
    eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store(), false, nullptr,
                                                  {}, journal);
    const auto replay = dispatcher.ExecuteCommand("gimbal.motion", "1.0.0", "gimbal-0",
                                                  nlohmann::json::object(), "invalid-command-1");
    EXPECT_EQ(replay.status, invalid.status);
    EXPECT_EQ(replay.payload, invalid.payload);
    EXPECT_EQ(replay.message, invalid.message);
  }
}

TEST(PrimitiveDispatchTest, ResolvesAbsoluteNavigationToBackendRelativeGoal) {
  auto registry = DemoRobot();
  eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store());
  ExpectAccepted(
      Exec(
          dispatcher, "base.navigate", "base-0",
          {{"reference", "relative"}, {"x_m", 1.0}, {"y_m", 0.0}, {"yaw_rad", 1.5707963267948966}}),
      "base.navigate");
  ExpectAccepted(Exec(dispatcher, "base.navigate", "base-0",
                      {{"reference", "absolute"},
                       {"frame_id", "map"},
                       {"x_m", 1.0},
                       {"y_m", 1.0},
                       {"yaw_rad", 3.141592653589793}}),
                 "base.navigate");
  const auto state = BaseState(registry);
  EXPECT_NEAR(state.x_m, 1.0, 1e-12);
  EXPECT_NEAR(state.y_m, 1.0, 1e-12);
  EXPECT_NEAR(state.yaw_rad, 3.141592653589793, 1e-12);
}

TEST(PrimitiveDispatchTest, ResolvesStreamTargetsFromFreshLocalizedPose) {
  eos::hardware::BaseTelemetry pose;
  pose.x_m = 2.0;
  pose.y_m = -1.0;
  pose.yaw_rad = 1.5707963267948966;
  pose.localization_valid = true;
  const auto relative =
      eos::runtime::dispatch::detail::ResolveBaseStreamMapTarget(pose, false, 0.3, 0.2, 0.25);
  ASSERT_TRUE(relative.has_value());
  EXPECT_NEAR((*relative)[0], 1.8, 1e-12);
  EXPECT_NEAR((*relative)[1], -0.7, 1e-12);
  EXPECT_NEAR((*relative)[2], pose.yaw_rad + 0.25, 1e-12);
  const auto absolute =
      eos::runtime::dispatch::detail::ResolveBaseStreamMapTarget(pose, true, 3.0, 4.0, -0.5);
  ASSERT_TRUE(absolute.has_value());
  EXPECT_EQ(*absolute, (std::array<double, 3>{3.0, 4.0, -0.5}));
  pose.fresh = false;
  EXPECT_FALSE(
      eos::runtime::dispatch::detail::ResolveBaseStreamMapTarget(pose, false, 0.3, 0.2, 0.25)
          .has_value());
  pose.fresh = true;
  pose.localization_valid = false;
  EXPECT_FALSE(
      eos::runtime::dispatch::detail::ResolveBaseStreamMapTarget(pose, true, 3.0, 4.0, -0.5)
          .has_value());
}

TEST(PrimitiveDispatchTest, RejectsReferenceDisabledByDeployment) {
  const char* binding = R"toml(
[[profile.test.device]]
class = "base"

[[profile.test.device.primitive]]
name = "base.navigate"
version = "1.0.0"
description = "relative navigation only"
references = ["relative"]
limits = { x_m = { minimum = -10.0, maximum = 10.0 }, y_m = { minimum = -10.0, maximum = 10.0 }, yaw_rad = { minimum = -6.28, maximum = 6.28 } }

[profile.test.device.backend]
type = "demo"

[profile.test.device.feeds.proprioception]
max_rate_hz = 100.0

[profile.test.device.feeds.health]
max_rate_hz = 10.0
)toml";
  auto registry = eos::runtime::hardware_runtime::Registry::Adopt(eos::backends::BuildBuiltinBackendCatalog(),
      eos::runtime::hardware_runtime::ParseBinding(binding, "relative-only.toml", "test", eos::backends::BuildBuiltinBackendCatalog()));
  eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store());
  ExpectRejected(Exec(dispatcher, "base.navigate", "base-0",
                      {{"reference", "absolute"},
                       {"frame_id", "map"},
                       {"x_m", 1.0},
                       {"y_m", 0.0},
                       {"yaw_rad", 0.0}}),
                 "base.navigate", "rejected.unsupported_reference");
}

TEST(PrimitiveDispatchTest, ArmInitReleasesConfiguredJointPose) {
  const char* binding = R"toml(
[[profile.test.device]]
class = "arm"
joint_names = ["joint_1", "joint_2", "joint_3", "joint_4", "joint_5", "joint_6"]

[profile.test.device.part]
initial_joint_positions_rad = [-0.1, -1.4, 1.4, 0.08, 1.5, 0.0]

[[profile.test.device.primitive]]
name = "arm.init"
version = "1.0.0"
description = "Return the test arm to its initial pose."
tier = "operator"

[profile.test.device.backend]
type = "demo"

[profile.test.device.feeds.proprioception]
max_rate_hz = 500.0

[profile.test.device.feeds.health]
max_rate_hz = 10.0
)toml";
  auto registry = eos::runtime::hardware_runtime::Registry::Adopt(eos::backends::BuildBuiltinBackendCatalog(),
      eos::runtime::hardware_runtime::ParseBinding(binding, "arm-init.toml", "test", eos::backends::BuildBuiltinBackendCatalog()));
  eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store());
  ExpectAccepted(Exec(dispatcher, "arm.init", "arm-0"), "arm.init");
  const auto* arm = &registry.Find(eos::hardware::DeviceKind::kManipulator);
  ASSERT_NE(arm, nullptr);
  const auto state = arm->feeds().ReadManipulatorTelemetry().value();
  const double expected[] = {-0.1, -1.4, 1.4, 0.08, 1.5, 0.0};
  for (std::size_t index = 0; index < 6; ++index) {
    EXPECT_DOUBLE_EQ(state.joint_angles_rad[index], expected[index]);
  }
}

TEST(PrimitiveDispatchTest, VelocityLeaseAndTargetedStopClamp) {
  auto registry = DemoRobotWithBaseCapabilities(true);
  eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store());
  ExpectAccepted(Exec(dispatcher, "base.velocity", "base-0",
                      {{"linear_x_m_s", 0.5},
                       {"linear_y_m_s", 0.0},
                       {"angular_z_rad_s", 0.0},
                       {"valid_for_ms", 100}}),
                 "base.velocity");
  dispatcher.Tick(0.1);
  registry.Tick(1.0);
  EXPECT_DOUBLE_EQ(BaseState(registry).linear_x_m_s, 0.0);
  ExpectAccepted(Exec(dispatcher, "motion.stop", "arm-0"), "motion.stop");
  ExpectRejected(Exec(dispatcher, "motion.stop", "speaker-0"), "motion.stop",
                 "rejected.invalid_target");
}

TEST(PrimitiveDispatchTest, StopRejectsWhenBackendCannotReleaseMotion) {
  auto registry = DemoRobot();
  eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store());
  const auto* arm = registry.FindDevice("arm-0");
  ASSERT_NE(arm, nullptr);
  const_cast<eos::hardware::Backend&>(arm->feeds()).Halt();

  ExpectRejected(Exec(dispatcher, "motion.stop", "arm-0"), "motion.stop", "rejected.invalid_state");
}

TEST(PrimitiveDispatchTest, DispatcherDisconnectClampsLeasedBase) {
  auto registry = DemoRobotWithBaseCapabilities(true);
  {
    eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store());
    ExpectAccepted(Exec(dispatcher, "base.velocity", "base-0",
                        {{"linear_x_m_s", 0.5},
                         {"linear_y_m_s", 0.0},
                         {"angular_z_rad_s", 0.0},
                         {"valid_for_ms", 1000}}),
                   "base.velocity");
    ExpectAccepted(Exec(dispatcher, "gripper.motion", "gripper-0", {{"action", "close"}}),
                   "gripper.motion");
  }
  registry.Tick(1.0);
  EXPECT_DOUBLE_EQ(BaseState(registry).linear_x_m_s, 0.0);
  EXPECT_DOUBLE_EQ(GripperState(registry).force_n.value_or(0.0), 0.0);
}

TEST(PrimitiveDispatchTest, EstopIsFleetWideAndFailClosed) {
  auto registry = DemoRobot();
  eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store());
  ExpectRejected(Exec(dispatcher, "safety.estop", "base-0"), "safety.estop",
                 "rejected.invalid_arguments");
  ExpectAccepted(Exec(dispatcher, "safety.estop", ""), "safety.estop");
  ExpectRejected(Exec(dispatcher, "base.posture", "base-0", {{"posture", "stand"}}), "base.posture",
                 "rejected.estop");
}

TEST(PrimitiveDispatchTest, RobotStopFansOutAndReplaysItsTerminalAggregate) {
  auto registry = DemoRobotWithBaseCapabilities(true);
  eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store());
  ExpectAccepted(Exec(dispatcher, "base.velocity", "base-0",
                      {{"linear_x_m_s", 0.5},
                       {"linear_y_m_s", 0.0},
                       {"angular_z_rad_s", 0.0},
                       {"valid_for_ms", 1000}}),
                 "base.velocity");

  const auto first = dispatcher.ExecuteCommand("robot.stop", "1.0.0", "", nlohmann::json::object(),
                                               "robot-stop-1");
  ASSERT_EQ(first.status, eos::runtime::dispatch::DispatchResult::Status::kCompleted);
  ExpectAccepted(first.payload, "robot.stop");
  EXPECT_EQ(first.payload.at("result").at("status"), "succeeded");
  EXPECT_EQ(first.payload.at("result").at("parts").size(), 3u);
  EXPECT_DOUBLE_EQ(BaseState(registry).linear_x_m_s, 0.0);

  const auto replay = dispatcher.ExecuteCommand("robot.stop", "1.0.0", "", nlohmann::json::object(),
                                                "robot-stop-1");
  EXPECT_EQ(replay.payload, first.payload);
  ExpectRejected(Exec(dispatcher, "robot.stop", "base-0"), "robot.stop",
                 "rejected.invalid_arguments");
}

TEST(PrimitiveDispatchTest, RobotStopKeepsAdmittedFailureDetailsInsideResult) {
  auto registry = DemoRobot();
  const auto* arm = registry.FindDevice("arm-0");
  ASSERT_NE(arm, nullptr);
  const_cast<eos::hardware::Backend&>(arm->feeds()).Halt();
  eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store());

  const nlohmann::json stopped = Exec(dispatcher, "robot.stop", "");
  ExpectAccepted(stopped, "robot.stop");
  EXPECT_EQ(stopped.at("result").at("status"), "failed");
  EXPECT_TRUE(stopped.at("reason").is_null());
  EXPECT_TRUE(stopped.at("reason_code").is_null());
  const auto failed =
      std::find_if(stopped.at("result").at("parts").begin(), stopped.at("result").at("parts").end(),
                   [](const auto& part) { return part.at("status") != "succeeded"; });
  ASSERT_NE(failed, stopped.at("result").at("parts").end());
  EXPECT_EQ(failed->at("device_id"), "arm-0");
}

TEST(PrimitiveDispatchTest, SafetyInterruptsBlockedModelIo) {
  BlockingTtsServer server;
  eos::runtime::models::ModelConfig tts;
  tts.id = "audio8";
  tts.capability = eos::runtime::models::Capability::kTts;
  tts.endpoint = server.Endpoint("/synthesize");
  tts.health_endpoint = server.Endpoint("/health");
  tts.health_interval = std::chrono::hours(1);
  tts.provider_model_id = "audio8";
  tts.request_contract = "tts-request";
  tts.response_contract = "tts-response";
  eos::runtime::models::Runtime models({tts}, [](const std::string&, const nlohmann::json&) {});
  for (int attempt = 0; attempt < 200; ++attempt) {
    const auto health = models.Health("audio8");
    if (health && health->status == eos::runtime::models::HealthStatus::kReady) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  const auto health = models.Health("audio8");
  ASSERT_TRUE(health.has_value());
  ASSERT_EQ(health->status, eos::runtime::models::HealthStatus::kReady);

  auto registry = DemoRobotWithBaseCapabilities(true);
  eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store(), false, &models);
  ExpectAccepted(Exec(dispatcher, "base.velocity", "base-0",
                      {{"linear_x_m_s", 0.5},
                       {"linear_y_m_s", 0.0},
                       {"angular_z_rad_s", 0.0},
                       {"valid_for_ms", 100}}),
                 "base.velocity");

  auto speaking = std::async(std::launch::async, [&] {
    return Exec(dispatcher, "speaker.speak", "speaker-0", {{"text", "blocked"}});
  });
  ASSERT_TRUE(server.WaitForSynthesis());

  auto independent_command = std::async(std::launch::async, [&] {
    return Exec(dispatcher, "base.posture", "base-0", {{"posture", "walk"}});
  });
  const auto independent_status = independent_command.wait_for(std::chrono::milliseconds(250));
  EXPECT_EQ(independent_status, std::future_status::ready);
  if (independent_status == std::future_status::ready) {
    ExpectAccepted(independent_command.get(), "base.posture");
  }

  auto lease_expiry = std::async(std::launch::async, [&] { dispatcher.Tick(0.1); });
  EXPECT_EQ(lease_expiry.wait_for(std::chrono::milliseconds(250)), std::future_status::ready);
  auto estop = std::async(std::launch::async, [&] { return Exec(dispatcher, "safety.estop", ""); });
  EXPECT_EQ(estop.wait_for(std::chrono::milliseconds(250)), std::future_status::ready);
  if (estop.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
    ExpectAccepted(estop.get(), "safety.estop");
  }
  EXPECT_DOUBLE_EQ(BaseState(registry).linear_x_m_s, 0.0);

  server.ReleaseSynthesis();
  EXPECT_FALSE(speaking.get().at("accepted").get<bool>());
}

TEST(PrimitiveDispatchTest, UnavailableDeviceDoesNotLatchOtherDevices) {
  auto registry = DemoRobot();
  eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store());
  const auto* arm = registry.FindDevice("arm-0");
  ASSERT_NE(arm, nullptr);
  const_cast<eos::hardware::Backend&>(arm->feeds()).Halt();

  ExpectAccepted(Exec(dispatcher, "base.posture", "base-0", {{"posture", "stand"}}),
                 "base.posture");
}

TEST(PrimitiveDispatchTest, ExplicitTargetAdmissionRejectsMissingUnknownAndWrongDevices) {
  auto registry = DemoRobot();
  eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store());
  ExpectRejected(Exec(dispatcher, "base.posture", "", {{"posture", "stand"}}), "base.posture",
                 "rejected.invalid_target");
  ExpectRejected(Exec(dispatcher, "base.posture", "missing", {{"posture", "stand"}}),
                 "base.posture", "rejected.invalid_target");
  ExpectRejected(Exec(dispatcher, "base.posture", "arm-0", {{"posture", "stand"}}), "base.posture",
                 "rejected.invalid_target");
  const auto unknown = dispatcher.Execute("arm.wave", "1.0.0", "arm-0", nlohmann::json::object());
  EXPECT_EQ(unknown.status, eos::runtime::dispatch::DispatchResult::Status::kNotFound);
}

TEST(PrimitiveDispatchTest, DefensivelyRejectsInvalidArgumentsForEveryDevicePrimitive) {
  auto registry = DemoRobotWithBaseCapabilities(true);
  eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store());
  ExpectInvalid(dispatcher, "base.velocity", "base-0",
                {{"linear_x_m_s", 1.1},
                 {"linear_y_m_s", 0.0},
                 {"angular_z_rad_s", 0.0},
                 {"valid_for_ms", 100}});
  ExpectInvalid(dispatcher, "base.posture", "base-0", {{"posture", "sit"}});
  ExpectInvalid(dispatcher, "base.navigate", "base-0",
                {{"reference", "relative"}, {"x_m", 10.1}, {"y_m", 0.0}, {"yaw_rad", 0.0}});
  ExpectInvalid(dispatcher, "gimbal.motion", "gimbal-0",
                {{"reference", "absolute"}, {"pan_rad", 0.1}});
  ExpectInvalid(dispatcher, "arm.movej", "arm-0",
                {{"reference", "absolute"}, {"joint_positions_rad", {0.5, 0.5}}});
  ExpectInvalid(dispatcher, "arm.movel", "arm-0",
                {{"reference", "relative"}, {"position_m", {0.1, 0.2, 0.3}}});
  ExpectInvalid(dispatcher, "arm.movej", "arm-0",
                {{"reference", "absolute"},
                 {"joint_positions_rad", {0.5, 0.5, 0.5, 0.5, 0.5, 0.5}},
                 {"speed_rad_s", 0.0}});
  ExpectInvalid(dispatcher, "gripper.motion", "gripper-0", {{"action", "toggle"}});
  ExpectInvalid(dispatcher, "gripper.motion", "gripper-0",
                {{"action", "close"}, {"force_n", 11.0}});
  ExpectInvalid(dispatcher, "speaker.play", "speaker-0", {{"uri", "asset://../outside.wav"}});
}

TEST(PrimitiveDispatchTest, SpeakRejectsAnUnavailableTtsDependency) {
  auto registry = DemoRobot();
  eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store());
  ExpectRejected(Exec(dispatcher, "speaker.speak", "speaker-0", {{"text", "hello"}}),
                 "speaker.speak", "rejected.unavailable_dependency");
}

TEST(PrimitiveDispatchTest, SchemaValidationPrecedesBackendAdmission) {
  auto registry = DemoRobot();
  eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store());
  const auto result = dispatcher.Execute("base.posture", "1.0.0", "base-0",
                                         {{"type", "arm.movej"}, {"posture", "stand"}});
  EXPECT_EQ(result.status, eos::runtime::dispatch::DispatchResult::Status::kInvalidArgument);
  EXPECT_TRUE(result.payload.is_null());
}

TEST(PrimitiveDispatchTest, OmittedOptionalValueWorksWithoutAnAdvertisedRange) {
  const char* binding = R"toml(
[[profile.test.device]]
class = "gripper"

[[profile.test.device.primitive]]
name = "gripper.motion"
version = "1.0.0"
description = "configured default force only"

[profile.test.device.backend]
type = "demo"

[profile.test.device.feeds.proprioception]
max_rate_hz = 50.0

[profile.test.device.feeds.health]
max_rate_hz = 10.0
)toml";
  auto registry = eos::runtime::hardware_runtime::Registry::Adopt(eos::backends::BuildBuiltinBackendCatalog(),
      eos::runtime::hardware_runtime::ParseBinding(binding, "default-force.toml", "test", eos::backends::BuildBuiltinBackendCatalog()));
  eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store());
  ExpectAccepted(Exec(dispatcher, "gripper.motion", "gripper-0", {{"action", "close"}}),
                 "gripper.motion");
  ExpectRejected(
      Exec(dispatcher, "gripper.motion", "gripper-0", {{"action", "close"}, {"force_n", 5.0}}),
      "gripper.motion", "rejected.invalid_arguments");
}

TEST(PrimitiveDispatchTest, GripperV2RejectsUnadmittedOrMismatchedOverrides) {
  const char* binding = R"toml(
[[profile.test.device]]
class = "gripper"

[[profile.test.device.primitive]]
name = "gripper.motion"
version = "1.0.0"
description = "bounded end-stop grasp"
limits = { opening_pct = { minimum = 0.0, maximum = 100.0 }, speed_pct = { minimum = 1.0, maximum = 100.0 } }

[profile.test.device.backend]
type = "demo"

[profile.test.device.feeds.proprioception]
max_rate_hz = 50.0

[profile.test.device.feeds.health]
max_rate_hz = 10.0
)toml";
  auto registry = eos::runtime::hardware_runtime::Registry::Adopt(eos::backends::BuildBuiltinBackendCatalog(),
      eos::runtime::hardware_runtime::ParseBinding(binding, "gripper-v2.toml", "test", eos::backends::BuildBuiltinBackendCatalog()));
  eos::runtime::dispatch::Dispatcher dispatcher(registry, dispatch_test::Store());
  ExpectRejected(dispatcher
                     .Execute("gripper.motion", "1.0.0", "gripper-0",
                              {{"action", "open"}, {"opening_pct", 0.0}})
                     .payload,
                 "gripper.motion", "rejected.invalid_arguments", "1.0.0");
  ExpectRejected(dispatcher
                     .Execute("gripper.motion", "1.0.0", "gripper-0",
                              {{"action", "close"}, {"opening_pct", 50.0}})
                     .payload,
                 "gripper.motion", "rejected.invalid_arguments", "1.0.0");
  ExpectRejected(dispatcher
                     .Execute("gripper.motion", "1.0.0", "gripper-0",
                              {{"action", "close"}, {"speed_pct", 101.0}})
                     .payload,
                 "gripper.motion", "rejected.invalid_arguments", "1.0.0");
  ExpectRejected(
      dispatcher
          .Execute("gripper.motion", "1.0.0", "gripper-0", {{"action", "close"}, {"force_n", 5.0}})
          .payload,
      "gripper.motion", "rejected.invalid_arguments", "1.0.0");
}

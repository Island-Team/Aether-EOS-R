/// Drives the ALSA microphone backend through a scripted capture stream, so
/// every case runs without a sound card: device presence, absence, a stalled
/// stream, halt, and the command paths a feed-only device must reject.
#include "alsa_mic.h"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace eos::backends::alsa {
namespace {

/// What a scripted open observed, so a test can assert the configuration
/// reached the device.
struct OpenRecord {
  std::mutex mutex;
  int calls{0};
  std::string alsa_device;
  std::uint32_t sample_rate_hz{0};
  std::uint16_t channels{0};
  std::uint32_t window_frames{0};
};

struct Script {
  /// Windows served before the stream ends; `kEndless` keeps serving.
  static constexpr int kEndless = -1;
  int windows{kEndless};
  bool open_succeeds{true};
  std::atomic<bool> stopped{false};
  std::atomic<int> reads{0};
  std::shared_ptr<OpenRecord> record{std::make_shared<OpenRecord>()};
};

/// Declares a capture stream that serves a fixed ramp, `index * 512`, so the
/// published window is checkable sample by sample.
CaptureOpen ScriptedOpen(const std::shared_ptr<Script>& script) {
  return [script](const AlsaMicConfig& config, CaptureStream* stream) {
    {
      std::lock_guard<std::mutex> lock(script->record->mutex);
      ++script->record->calls;
      script->record->alsa_device = config.alsa_device;
      script->record->sample_rate_hz = config.sample_rate_hz;
      script->record->channels = config.channels;
      script->record->window_frames = config.window_frames;
    }
    if (!script->open_succeeds) return false;
    const std::uint32_t window_frames = config.window_frames;
    stream->read = [script, window_frames](std::vector<std::int16_t>* frames,
                                           std::size_t frames_wanted) {
      const int served = script->reads.fetch_add(1);
      if (script->windows != Script::kEndless && served >= script->windows) return false;
      const std::size_t count = std::min<std::size_t>(window_frames, frames_wanted);
      frames->assign(count, 0);
      for (std::size_t index = 0; index < count; ++index) {
        (*frames)[index] = static_cast<std::int16_t>(index * 512);
      }
      return true;
    };
    stream->stop = [script] { script->stopped = true; };
    return true;
  };
}

/// Polls until the backend publishes a window, or the deadline passes.
std::optional<hardware::MicrophoneTelemetry> WaitForWindow(const AlsaMic& mic,
                                                           std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (auto telemetry = mic.ReadMicrophoneTelemetry()) return telemetry;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return mic.ReadMicrophoneTelemetry();
}

AlsaMicConfig Config() {
  AlsaMicConfig config;
  config.device_id = "alsa-mic-0";
  config.alsa_device = "default";
  config.sample_rate_hz = 16000;
  config.channels = 1;
  config.window_frames = 64;
  config.read_timeout_ms = 100;
  return config;
}

TEST(AlsaMicTest, PublishesTheCapturedWindowThroughNeutralTelemetry) {
  const auto script = std::make_shared<Script>();
  AlsaMic mic(Config(), ScriptedOpen(script));

  const hardware::BackendIdentity identity = mic.Identify();
  EXPECT_EQ(identity.name, "alsa-mic");
  EXPECT_EQ(identity.kind, hardware::DeviceKind::kMicrophone);
  EXPECT_EQ(identity.device_id, "alsa-mic-0");
  const hardware::FeedDeclarations feeds = mic.DeclareFeeds();
  EXPECT_DOUBLE_EQ(feeds.sensor.max_rate_hz, 250.0);

  const std::optional<hardware::MicrophoneTelemetry> telemetry =
      WaitForWindow(mic, std::chrono::seconds(3));
  ASSERT_TRUE(telemetry.has_value());
  EXPECT_EQ(telemetry->sample_rate_hz, 16000u);
  EXPECT_EQ(telemetry->channels, 1u);
  EXPECT_EQ(telemetry->frame_id, "mic");
  ASSERT_GE(telemetry->pcm.size(), 64u);
  ASSERT_EQ(telemetry->pcm.size() % 64u, 0u);
  for (std::size_t offset = 0; offset < telemetry->pcm.size(); offset += 64) {
    EXPECT_EQ(telemetry->pcm[offset], 0);
    EXPECT_EQ(telemetry->pcm[offset + 1], 512);
    EXPECT_EQ(telemetry->pcm[offset + 63], 63 * 512);
  }
  EXPECT_TRUE(mic.ReadHealth().alive);

  // The configured device reached the capture side unaltered.
  std::lock_guard<std::mutex> lock(script->record->mutex);
  EXPECT_EQ(script->record->calls, 1);
  EXPECT_EQ(script->record->alsa_device, "default");
  EXPECT_EQ(script->record->sample_rate_hz, 16000u);
  EXPECT_EQ(script->record->channels, 1u);
  EXPECT_EQ(script->record->window_frames, 64u);
}

TEST(AlsaMicTest, AnUnopenableDeviceIsAbsenceRatherThanAnEmptyWindow) {
  const auto script = std::make_shared<Script>();
  script->open_succeeds = false;
  AlsaMic mic(Config(), ScriptedOpen(script));

  // Give the worker a chance to attempt the open.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(mic.ReadMicrophoneTelemetry().has_value());
  const hardware::BackendHealth health = mic.ReadHealth();
  EXPECT_FALSE(health.alive);
  EXPECT_FALSE(health.admitted);
}

TEST(AlsaMicTest, AStreamThatEndsStopsReportingAWindow) {
  const auto script = std::make_shared<Script>();
  script->windows = 1;
  AlsaMic mic(Config(), ScriptedOpen(script));

  // The single window is produced and then the stream ends. A dead device must
  // not keep serving its last window, so the end state is absence, not a
  // repeated sample. Wait on the teardown itself: health is also not-alive
  // before the first window arrives, so it cannot mark this transition.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline && !script->stopped.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_TRUE(script->stopped.load());
  EXPECT_FALSE(mic.ReadMicrophoneTelemetry().has_value());
  EXPECT_FALSE(mic.ReadHealth().alive);
}

TEST(AlsaMicTest, ClampLeavesAContinuousCaptureAliveAndHaltStopsIt) {
  const auto script = std::make_shared<Script>();
  AlsaMic mic(Config(), ScriptedOpen(script));
  ASSERT_TRUE(WaitForWindow(mic, std::chrono::seconds(3)).has_value());

  // A microphone has no actuation path and no last safe position, so a clamp is
  // a no-op: capture continues.
  mic.Clamp();
  EXPECT_TRUE(mic.ReadHealth().alive);
  EXPECT_FALSE(script->stopped.load());
  EXPECT_TRUE(WaitForWindow(mic, std::chrono::seconds(3)).has_value());

  mic.Halt();
  EXPECT_FALSE(mic.ReadMicrophoneTelemetry().has_value());
  EXPECT_FALSE(mic.ReadHealth().alive);
}

TEST(AlsaMicTest, AFeedOnlyDeviceRejectsEveryCommandPath) {
  const auto script = std::make_shared<Script>();
  AlsaMic mic(Config(), ScriptedOpen(script));

  hardware::MoveJCommand move_j{};
  hardware::MoveLCommand move_l{};
  hardware::BaseCommand base{};
  hardware::GimbalCommand gimbal{};
  hardware::GripperCommand gripper{};
  hardware::SpeakerCommand speaker{};
  hardware::BaseNavigateRelativeCommand navigation{};
  EXPECT_EQ(mic.ExecuteMoveJ(move_j), hardware::ReleaseResult::kRejected);
  EXPECT_EQ(mic.ExecuteMoveL(move_l), hardware::ReleaseResult::kRejected);
  EXPECT_EQ(mic.ExecuteBase(base), hardware::ReleaseResult::kRejected);
  EXPECT_EQ(mic.ExecuteGimbal(gimbal), hardware::ReleaseResult::kRejected);
  EXPECT_EQ(mic.ExecuteGripper(gripper), hardware::ReleaseResult::kRejected);
  EXPECT_EQ(mic.ExecuteSpeaker(speaker), hardware::ReleaseResult::kRejected);
  EXPECT_EQ(mic.ExecuteNavigateRelative(navigation), hardware::ReleaseResult::kRejected);
  EXPECT_EQ(mic.ExecutePosture({hardware::BasePosture::kStand, "posture"}),
            hardware::ReleaseResult::kRejected);
  EXPECT_EQ(mic.StopMotion(), hardware::ReleaseResult::kRejected);
}

TEST(AlsaMicTest, RejectsAnIncompleteConfiguration) {
  AlsaMicConfig empty;
  EXPECT_THROW(
      { AlsaMic mic(empty, ScriptedOpen(std::make_shared<Script>())); }, std::invalid_argument);

  AlsaMicConfig zero_window = Config();
  zero_window.window_frames = 0;
  EXPECT_THROW(
      { AlsaMic mic(zero_window, ScriptedOpen(std::make_shared<Script>())); },
      std::invalid_argument);
}

/// Puts `directory` first on PATH and points the stand-in at `pcm_path`, then
/// restores both on scope exit.
class ScopedPathOverride {
 public:
  ScopedPathOverride(const std::filesystem::path& directory, const std::filesystem::path& pcm) {
    const char* existing = std::getenv("PATH");
    previous_path_ = existing != nullptr ? existing : "";
    ::setenv("PATH", (directory.string() + ":" + previous_path_).c_str(), 1);
    ::setenv("EOS_FAKE_ARECORD_PCM", pcm.c_str(), 1);
  }
  ~ScopedPathOverride() {
    ::setenv("PATH", previous_path_.c_str(), 1);
    ::unsetenv("EOS_FAKE_ARECORD_PCM");
  }
  ScopedPathOverride(const ScopedPathOverride&) = delete;
  ScopedPathOverride& operator=(const ScopedPathOverride&) = delete;

 private:
  std::string previous_path_;
};

/// A temp directory holding an `arecord` stand-in that replays `pcm` and then
/// holds the pipe open, so the published window stays observable before the
/// stream is torn down.
std::filesystem::path MakeArecordStandIn(const std::filesystem::path& pcm) {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / ("eos-alsa-mic-test-" + std::to_string(::getpid()));
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  const std::filesystem::path script = directory / "arecord";
  {
    std::ofstream out(script);
    out << "#!/bin/sh\n"
           "# Ignores its arguments: the test supplies the samples.\n"
           "cat \"$EOS_FAKE_ARECORD_PCM\"\n"
           "sleep 5\n";
  }
  ::chmod(script.c_str(), 0755);
  return directory;
}

/// The little-endian 16-bit ramp a stand-in device replays: `index * 512`.
std::filesystem::path WriteRamp(std::size_t frames) {
  const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                     ("eos-alsa-mic-ramp-" + std::to_string(::getpid()) + ".bin");
  std::string bytes;
  for (std::size_t index = 0; index < frames; ++index) {
    const std::uint16_t value = static_cast<std::uint16_t>(index * 512);
    bytes.push_back(static_cast<char>(value & 0xff));
    bytes.push_back(static_cast<char>(value >> 8));
  }
  std::ofstream out(path, std::ios::binary);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return path;
}

TEST(AlsaMicTest, TheRealArecordPathIsExercisedThroughAPathStandIn) {
  const std::filesystem::path pcm = WriteRamp(128);
  const std::filesystem::path directory = MakeArecordStandIn(pcm);
  {
    const ScopedPathOverride path_override(directory, pcm);
    AlsaMicConfig config = Config();
    config.read_timeout_ms = 1000;
    // No injected capture here: this drives the real spawn, the pipe, and the
    // window assembly, which is the half of the backend a sound card would
    // otherwise gate. The stand-in replaces only the audio source.
    AlsaMic mic(config);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const std::optional<hardware::MicrophoneTelemetry> telemetry =
        WaitForWindow(mic, std::chrono::seconds(3));
    ASSERT_TRUE(telemetry.has_value());
    ASSERT_EQ(telemetry->pcm.size(), 128u);
    EXPECT_EQ(telemetry->pcm[0], 0);
    EXPECT_EQ(telemetry->pcm[1], 512);
    EXPECT_EQ(telemetry->pcm[63], 63 * 512);
    EXPECT_EQ(telemetry->pcm[127], static_cast<std::int16_t>(127 * 512));
    EXPECT_TRUE(mic.ReadHealth().alive);
    EXPECT_EQ(mic.Identify().name, "alsa-mic");
  }
  std::filesystem::remove_all(directory);
  std::filesystem::remove(pcm);
}

TEST(AlsaMicTest, TheRealArecordPathReportsAbsenceForAnUnknownDevice) {
  AlsaMicConfig config = Config();
  config.alsa_device = "eos-no-such-device";
  config.read_timeout_ms = 500;
  // A device that cannot be opened must surface as absence, and must do so
  // without ever publishing a window. This runs the real arecord, so it holds
  // on a host with no sound card and on one with several.
  AlsaMic mic(config);
  bool ever_had_a_window = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    ever_had_a_window = ever_had_a_window || mic.ReadMicrophoneTelemetry().has_value();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  EXPECT_FALSE(ever_had_a_window);
  EXPECT_FALSE(mic.ReadHealth().alive);
}

}  // namespace
}  // namespace eos::backends::alsa

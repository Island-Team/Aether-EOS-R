#include "src/eos/ext/backends/aplay/aplay_speaker.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace eos::backends::aplay {
namespace {

void AppendLe16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}

void AppendLe32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    bytes.push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

std::vector<std::uint8_t> Wave() {
  std::vector<std::uint8_t> bytes{'R', 'I', 'F', 'F'};
  AppendLe32(bytes, 40);
  bytes.insert(bytes.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  AppendLe32(bytes, 16);
  AppendLe16(bytes, 1);
  AppendLe16(bytes, 1);
  AppendLe32(bytes, 16000);
  AppendLe32(bytes, 32000);
  AppendLe16(bytes, 2);
  AppendLe16(bytes, 16);
  bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
  AppendLe32(bytes, 4);
  bytes.insert(bytes.end(), {0, 0, 1, 0});
  return bytes;
}

hardware::SpeakerCommand Asset(std::string id, std::vector<std::uint8_t> data,
                               std::string media_type = "audio/wav") {
  return {.asset_id = std::move(id), .media_type = std::move(media_type), .data = std::move(data)};
}

template <typename Predicate>
bool Wait(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!predicate() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return predicate();
}

class Harness {
 public:
  Playback Player() {
    return [this](const std::vector<std::uint8_t>& bytes, const std::string& device,
                  std::chrono::milliseconds,
                  const CancellationProbe& cancelled) -> PlaybackOutcome {
      played = bytes;
      played_device = device;
      playback_started = true;
      if (playback_throws.load()) throw std::runtime_error("test playback failure");
      while (block_playback.load() && !cancelled()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      playback_finished = true;
      if (!playback_success.load() || cancelled()) return {false, playback_fail_reason};
      return {true, {}};
    };
  }

  std::atomic<bool> playback_success{true};
  std::atomic<bool> playback_throws{false};
  std::atomic<bool> block_playback{false};
  std::atomic<bool> playback_started{false};
  std::atomic<bool> playback_finished{false};
  // Written before the speaker and its worker exist, so no synchronisation is
  // needed around it.
  std::string playback_fail_reason{"test playback failed"};
  std::string played_device;
  std::vector<std::uint8_t> played;
};

AplaySpeaker MakeSpeaker(Harness& harness, std::size_t max_stream_bytes = 1024) {
  return AplaySpeaker({.device_id = "speaker-0",
                       .alsa_device = "test-device",
                       .max_stream_bytes = max_stream_bytes},
                      harness.Player());
}

TEST(AplaySpeakerTest, PlaysOneAtomicWaveAssetWithoutSynthesis) {
  Harness harness;
  AplaySpeaker speaker = MakeSpeaker(harness);
  const auto wave = Wave();
  EXPECT_EQ(speaker.ExecuteSpeaker(Asset("ready", wave)), hardware::ReleaseResult::kAccepted);
  ASSERT_TRUE(Wait([&] { return harness.playback_started.load(); }));
  ASSERT_TRUE(Wait([&] {
    return speaker.ReadSpeakerTelemetry()->state == hardware::SpeakerPlaybackState::kIdle;
  }));
  EXPECT_EQ(harness.played, wave);
  EXPECT_EQ(harness.played_device, "test-device");
  EXPECT_EQ(speaker.ReadSpeakerTelemetry()->asset_id, "ready");
  EXPECT_TRUE(speaker.ReadHealth().alive);
}

TEST(AplaySpeakerTest, RejectsMalformedMediaAndSize) {
  Harness harness;
  AplaySpeaker speaker = MakeSpeaker(harness, 64);
  EXPECT_EQ(speaker.ExecuteSpeaker(Asset("bad", Wave(), "audio/mpeg")),
            hardware::ReleaseResult::kRejected);
  EXPECT_EQ(speaker.ExecuteSpeaker(Asset("bad", {1, 2, 3})), hardware::ReleaseResult::kRejected);
  EXPECT_EQ(speaker.ExecuteSpeaker(Asset("large", std::vector<std::uint8_t>(65))),
            hardware::ReleaseResult::kRejected);
}

TEST(AplaySpeakerTest, PlaybackExceptionRecoversForAnotherStream) {
  Harness harness;
  harness.playback_throws = true;
  AplaySpeaker speaker = MakeSpeaker(harness);
  EXPECT_EQ(speaker.ExecuteSpeaker(Asset("first", Wave())), hardware::ReleaseResult::kAccepted);
  ASSERT_TRUE(Wait([&] {
    return speaker.ReadSpeakerTelemetry()->state == hardware::SpeakerPlaybackState::kFault;
  }));

  harness.playback_throws = false;
  harness.playback_started = false;
  EXPECT_EQ(speaker.ExecuteSpeaker(Asset("second", Wave())), hardware::ReleaseResult::kAccepted);
  ASSERT_TRUE(Wait([&] { return harness.playback_started.load(); }));
}

TEST(AplaySpeakerTest, ReportsPlaybackCauseUntilTheNextCommand) {
  Harness harness;
  harness.playback_success = false;
  harness.playback_fail_reason = "aplay exited with status 1";
  AplaySpeaker speaker = MakeSpeaker(harness);
  EXPECT_EQ(speaker.ExecuteSpeaker(Asset("first", Wave())), hardware::ReleaseResult::kAccepted);
  ASSERT_TRUE(Wait([&] {
    return speaker.ReadSpeakerTelemetry()->state == hardware::SpeakerPlaybackState::kFault;
  }));
  EXPECT_EQ(speaker.ReleaseFailureReason(), "aplay exited with status 1");

  harness.playback_success = true;
  harness.playback_started = false;
  EXPECT_EQ(speaker.ExecuteSpeaker(Asset("second", Wave())), hardware::ReleaseResult::kAccepted);
  ASSERT_TRUE(Wait([&] {
    return speaker.ReadSpeakerTelemetry()->state == hardware::SpeakerPlaybackState::kIdle;
  }));
  EXPECT_EQ(speaker.ReleaseFailureReason(), "aplay speaker refused the release");
}

TEST(AplaySpeakerTest, ClampCancelsPlaybackAndAllowsAnotherStream) {
  Harness harness;
  harness.block_playback = true;
  AplaySpeaker speaker = MakeSpeaker(harness);
  EXPECT_EQ(speaker.ExecuteSpeaker(Asset("first", Wave())), hardware::ReleaseResult::kAccepted);
  ASSERT_TRUE(Wait([&] { return harness.playback_started.load(); }));
  speaker.Clamp();
  ASSERT_TRUE(Wait([&] { return harness.playback_finished.load(); }));
  harness.block_playback = false;
  EXPECT_EQ(speaker.ExecuteSpeaker(Asset("second", Wave())), hardware::ReleaseResult::kAccepted);
}

TEST(AplaySpeakerTest, HaltCancelsAndLatchesHealth) {
  Harness harness;
  harness.block_playback = true;
  AplaySpeaker speaker = MakeSpeaker(harness);
  EXPECT_EQ(speaker.ExecuteSpeaker(Asset("first", Wave())), hardware::ReleaseResult::kAccepted);
  ASSERT_TRUE(Wait([&] { return harness.playback_started.load(); }));
  speaker.Halt();
  EXPECT_FALSE(speaker.ReadHealth().alive);
  EXPECT_EQ(speaker.ExecuteSpeaker(Asset("second", Wave())), hardware::ReleaseResult::kRejected);
}

}  // namespace
}  // namespace eos::backends::aplay

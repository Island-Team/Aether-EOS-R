#include "src/eos/runtime/models/microphone_vad.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <utility>

namespace eos::runtime::models {
namespace {

TEST(MicrophoneVadPumpTest, IgnoresUnsupportedAudioFormatWithoutCallingAModel) {
  Runtime runtime;
  MicrophoneVadStore store;
  {
    MicrophoneVadPump pump(
        runtime,
        [] {
          hardware::MicrophoneTelemetry block;
          block.sample_rate_hz = 48000;
          block.source_timestamp_ns = 1;
          block.pcm.assign(960, 12000);
          return std::optional<hardware::MicrophoneTelemetry>(std::move(block));
        },
        store, "vad", std::chrono::milliseconds(20), std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_FALSE(store.Latest());
}

}  // namespace
}  // namespace eos::runtime::models

#pragma once

#include <librealsense2/rs.hpp>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace eos::backends::vendor {

// Latest-frame-only EOS -> grasp transport. The D435 remains owned by EOS.
class WristGraspStream {
 public:
  WristGraspStream();
  ~WristGraspStream();
  WristGraspStream(const WristGraspStream&) = delete;
  WristGraspStream& operator=(const WristGraspStream&) = delete;

  void Queue(const rs2::video_frame& color, const rs2::depth_frame& depth,
             std::int64_t timestamp_ns, const std::vector<std::uint8_t>& jpeg);
  void Stop();

 private:
  struct Frame {
    rs2::video_frame color;
    rs2::depth_frame depth;
    std::int64_t timestamp_ns;
    std::vector<std::uint8_t> jpeg;
  };
  static std::vector<std::uint8_t> Encode(const Frame& frame);
  void SendLoop();

  std::mutex mutex_;
  std::condition_variable changed_;
  std::optional<Frame> pending_;
  std::thread worker_;
  bool stopped_{false};
};

}  // namespace eos::backends::vendor

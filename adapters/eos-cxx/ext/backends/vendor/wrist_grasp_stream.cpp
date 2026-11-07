#include "wrist_grasp_stream.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <chrono>
#include <cstring>
#include <memory>
#include <utility>

#include "adapters/eos-cxx/lib/log.h"

namespace eos::backends::vendor {
namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

void AppendLe(std::vector<std::uint8_t>* output, std::uint64_t value, int bytes) {
  for (int index = 0; index < bytes; ++index) {
    output->push_back(static_cast<std::uint8_t>(value >> (index * 8)));
  }
}
}  // namespace

WristGraspStream::WristGraspStream() : worker_([this] { SendLoop(); }) {}
WristGraspStream::~WristGraspStream() { Stop(); }

void WristGraspStream::Queue(const rs2::video_frame& color, const rs2::depth_frame& depth,
                            std::int64_t timestamp_ns,
                            const std::vector<std::uint8_t>& jpeg) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopped_) return;
  pending_ = Frame{color, depth, timestamp_ns, jpeg};
  changed_.notify_one();
}

void WristGraspStream::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
  }
  changed_.notify_all();
  if (worker_.joinable()) worker_.join();
}

std::vector<std::uint8_t> WristGraspStream::Encode(const Frame& frame) {
  rs2::pointcloud pointcloud;
  const rs2::points points = pointcloud.calculate(frame.depth);
  pointcloud.map_to(frame.color);
  const rs2::vertex* vertices = points.get_vertices();
  const int width = frame.depth.get_width();
  const int height = frame.depth.get_height();
  if (vertices == nullptr || width <= 0 || frame.jpeg.empty()) return {};
  std::vector<float> xyzuv;
  constexpr int kCloudPixelStride = 4;
  xyzuv.reserve((points.size() / (kCloudPixelStride * kCloudPixelStride)) * 5);
  for (int v = 0; v < height; v += kCloudPixelStride) {
    for (int u = 0; u < width; u += kCloudPixelStride) {
      const std::size_t index = static_cast<std::size_t>(v) * width + u;
      if (index >= points.size() || vertices[index].z == 0.0F) continue;
      xyzuv.push_back(vertices[index].x);
      xyzuv.push_back(vertices[index].y);
      xyzuv.push_back(vertices[index].z);
      xyzuv.push_back(static_cast<float>(u));
      xyzuv.push_back(static_cast<float>(v));
    }
  }
  if (xyzuv.empty()) return {};
  // ERG1, stamp, image dimensions, JPEG length, point count, JPEG, xyzuv float32 LE.
  std::vector<std::uint8_t> packet;
  packet.reserve(28 + frame.jpeg.size() + xyzuv.size() * sizeof(float));
  packet.insert(packet.end(), {'E', 'R', 'G', '1'});
  AppendLe(&packet, static_cast<std::uint64_t>(frame.timestamp_ns), 8);
  AppendLe(&packet, frame.color.get_width(), 4);
  AppendLe(&packet, frame.color.get_height(), 4);
  AppendLe(&packet, frame.jpeg.size(), 4);
  AppendLe(&packet, xyzuv.size() / 5, 4);
  packet.insert(packet.end(), frame.jpeg.begin(), frame.jpeg.end());
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(xyzuv.data());
  packet.insert(packet.end(), bytes, bytes + xyzuv.size() * sizeof(float));
  return packet;
}

void WristGraspStream::SendLoop() {
  asio::io_context io;
  std::unique_ptr<websocket::stream<beast::tcp_stream>> socket;
  auto last_error_log = std::chrono::steady_clock::time_point{};
  for (;;) {
    std::optional<Frame> frame;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      changed_.wait(lock, [this] { return stopped_ || pending_.has_value(); });
      if (stopped_) return;
      frame = std::move(pending_);
      pending_.reset();
    }
    try {
      if (!socket) {
        tcp::resolver resolver(io);
        socket = std::make_unique<websocket::stream<beast::tcp_stream>>(io);
        beast::get_lowest_layer(*socket).expires_after(std::chrono::seconds(2));
        beast::get_lowest_layer(*socket).connect(resolver.resolve("127.0.0.1", "18765"));
        socket->handshake("127.0.0.1:18765", "/v1/frames");
        socket->binary(true);
      }
      const auto packet = Encode(*frame);
      if (packet.empty()) continue;
      beast::get_lowest_layer(*socket).expires_after(std::chrono::seconds(2));
      socket->write(asio::buffer(packet));
    } catch (const std::exception& error) {
      socket.reset();
      const auto now = std::chrono::steady_clock::now();
      if (last_error_log.time_since_epoch().count() == 0 ||
          now - last_error_log >= std::chrono::seconds(10)) {
        eos::lib::LogEvent(eos::lib::GetLogger("vendor"), spdlog::level::warn,
                           "wrist_grasp_stream_failed", {{"reason", error.what()}});
        last_error_log = now;
      }
    }
  }
}

}  // namespace eos::backends::vendor

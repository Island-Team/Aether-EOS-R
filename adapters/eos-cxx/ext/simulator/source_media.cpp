#include "source_media.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fstream>
#include <optional>
#include <stdexcept>

namespace eos::ext::simulator {
namespace {

constexpr std::size_t kMaximumFrameBytes = 8U * 1024U * 1024U;
constexpr std::size_t kMaximumDecodedBytes = 64U * 1024U * 1024U;
constexpr std::size_t kMaximumFrames = 18'000;

std::vector<std::uint8_t> ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot read demo camera source: " + path.string());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

}  // namespace

std::optional<std::pair<std::uint32_t, std::uint32_t>> JpegDimensions(
    const std::vector<std::uint8_t>& bytes) {
  if (bytes.size() < 4 || bytes[0] != 0xff || bytes[1] != 0xd8) return std::nullopt;
  std::size_t offset = 2;
  while (offset + 4 <= bytes.size()) {
    while (offset < bytes.size() && bytes[offset] == 0xff) ++offset;
    if (offset >= bytes.size()) break;
    const std::uint8_t marker = bytes[offset++];
    if (marker == 0xd8 || marker == 0x01 || (marker >= 0xd0 && marker <= 0xd9)) continue;
    if (offset + 2 > bytes.size()) break;
    const std::size_t length = (static_cast<std::size_t>(bytes[offset]) << 8) | bytes[offset + 1];
    if (length < 2 || offset + length > bytes.size()) break;
    if ((marker >= 0xc0 && marker <= 0xc3) || (marker >= 0xc5 && marker <= 0xc7) ||
        (marker >= 0xc9 && marker <= 0xcb) || (marker >= 0xcd && marker <= 0xcf)) {
      if (length < 7) break;
      return std::pair{(static_cast<std::uint32_t>(bytes[offset + 5]) << 8) | bytes[offset + 6],
                       (static_cast<std::uint32_t>(bytes[offset + 3]) << 8) | bytes[offset + 4]};
    }
    offset += length;
  }
  return std::nullopt;
}

namespace {

std::vector<std::uint8_t> DecodeWithGStreamer(const std::filesystem::path& path, bool video) {
  const std::string location = "location=" + path.string();
  int output[2];
  if (::pipe(output) != 0) throw std::runtime_error("cannot create media decoder pipe");
  const pid_t child = ::fork();
  if (child < 0) {
    ::close(output[0]);
    ::close(output[1]);
    throw std::runtime_error("cannot start media decoder");
  }
  if (child == 0) {
    ::dup2(output[1], STDOUT_FILENO);
    ::close(output[0]);
    ::close(output[1]);
    if (video) {
      ::execl(EOS_GSTREAMER_LAUNCH, EOS_GSTREAMER_LAUNCH, "-q", "filesrc",
              location.c_str(), "!", "decodebin", "!", "videoconvert", "!",
              "videorate", "!", "video/x-raw,framerate=30/1", "!", "jpegenc", "quality=90", "!",
              "fdsink", "fd=1", static_cast<char*>(nullptr));
    } else {
      ::execl(EOS_GSTREAMER_LAUNCH, EOS_GSTREAMER_LAUNCH, "-q", "filesrc",
              location.c_str(), "!", "decodebin", "!", "videoconvert", "!",
              "jpegenc", "quality=90", "!", "fdsink", "fd=1", static_cast<char*>(nullptr));
    }
    _exit(127);
  }

  ::close(output[1]);
  std::vector<std::uint8_t> bytes;
  std::array<std::uint8_t, 64U * 1024U> chunk{};
  bool exceeded = false;
  while (true) {
    const ssize_t count = ::read(output[0], chunk.data(), chunk.size());
    if (count > 0) {
      if (bytes.size() + static_cast<std::size_t>(count) > kMaximumDecodedBytes) {
        exceeded = true;
        ::kill(child, SIGKILL);
        break;
      }
      bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + count);
      continue;
    }
    if (count < 0 && errno == EINTR) continue;
    break;
  }
  ::close(output[0]);
  int status = 0;
  pid_t waited = -1;
  do {
    waited = ::waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (exceeded) throw std::invalid_argument("decoded demo camera source exceeds 64 MiB");
  if (waited != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || bytes.empty())
    throw std::invalid_argument("demo camera source cannot be decoded: " + path.string());
  return bytes;
}

std::vector<EncodedFrame> SplitJpegs(const std::vector<std::uint8_t>& stream) {
  static constexpr std::array<std::uint8_t, 2> kStart{0xff, 0xd8};
  static constexpr std::array<std::uint8_t, 2> kEnd{0xff, 0xd9};
  std::vector<EncodedFrame> frames;
  std::size_t cursor = 0;
  while (cursor + 1 < stream.size()) {
    const auto start =
        std::search(stream.begin() + cursor, stream.end(), kStart.begin(), kStart.end());
    if (start == stream.end()) break;
    const auto end = std::search(start + 2, stream.end(), kEnd.begin(), kEnd.end());
    if (end == stream.end()) break;
    std::vector<std::uint8_t> jpeg(start, end + 2);
    const auto dimensions = JpegDimensions(jpeg);
    if (!dimensions || jpeg.size() > kMaximumFrameBytes)
      throw std::invalid_argument("decoded demo camera frame is invalid or exceeds 8 MiB");
    frames.push_back(
        {.width_px = dimensions->first, .height_px = dimensions->second, .jpeg = std::move(jpeg)});
    if (frames.size() > kMaximumFrames)
      throw std::invalid_argument("decoded demo camera source exceeds 18000 frames");
    cursor = static_cast<std::size_t>(std::distance(stream.begin(), end + 2));
  }
  if (frames.empty()) throw std::invalid_argument("demo camera source contains no video frames");
  return frames;
}

}  // namespace

DecodedMedia DecodeMedia(const std::string& id, const std::filesystem::path& path,
                         const std::string& media_type) {
  const bool video = media_type == "video/mp4" || media_type == "video/webm";
  std::vector<std::uint8_t> stream;
  if (media_type == "image/jpeg") {
    stream = ReadFile(path);
  } else if (media_type == "image/png" || media_type == "image/webp" || video) {
    stream = DecodeWithGStreamer(path, video);
  } else {
    throw std::invalid_argument("unsupported demo camera media type: " + media_type);
  }
  return {.id = id, .video = video, .frames_per_second = 30.0, .frames = SplitJpegs(stream)};
}

}  // namespace eos::ext::simulator

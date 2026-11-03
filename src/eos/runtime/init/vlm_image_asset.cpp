#include "vlm_image_asset.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string_view>

namespace eos::runtime::init {
namespace {
// Base64 and JSON framing must fit the configured 16 MiB VLM request limit.
constexpr std::size_t kMaxImageBytes = 8 * 1024 * 1024;
constexpr std::string_view kScheme = "asset://";

bool Contained(const std::filesystem::path& root, const std::filesystem::path& file) {
  return std::mismatch(root.begin(), root.end(), file.begin(), file.end()).first == root.end();
}
}  // namespace

std::vector<std::uint8_t> ReadVlmJpegAsset(const std::filesystem::path& root,
                                           const std::string& uri) {
  if (root.empty() || uri.rfind(kScheme, 0) != 0)
    throw std::invalid_argument("VLM image must use an asset:// URI");
  const std::string relative = uri.substr(kScheme.size());
  if (relative.empty() || relative.front() == '/' || relative.find('\\') != std::string::npos ||
      relative.find('%') != std::string::npos)
    throw std::invalid_argument("invalid VLM image asset path");
  const std::filesystem::path requested(relative);
  for (const auto& part : requested) {
    if (part == "." || part == "..")
      throw std::invalid_argument("invalid VLM image asset path");
  }
  std::error_code error;
  const auto canonical_root = std::filesystem::canonical(root, error);
  if (error) throw std::invalid_argument("VLM asset root is unavailable");
  const auto file = std::filesystem::canonical(canonical_root / requested, error);
  if (error || !Contained(canonical_root, file) ||
      !std::filesystem::is_regular_file(file, error) || error ||
      (file.extension() != ".jpg" && file.extension() != ".jpeg"))
    throw std::invalid_argument("VLM image must be a JPEG under the EOS asset root");
  const auto size = std::filesystem::file_size(file, error);
  if (error || size < 4 || size > kMaxImageBytes)
    throw std::invalid_argument("VLM JPEG asset size is invalid");
  std::ifstream input(file, std::ios::binary);
  if (!input) throw std::invalid_argument("VLM JPEG asset is unreadable");
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!input || input.peek() != std::ifstream::traits_type::eof() ||
      bytes[0] != 0xff || bytes[1] != 0xd8 || bytes[bytes.size() - 2] != 0xff ||
      bytes.back() != 0xd9)
    throw std::invalid_argument("VLM asset is not a valid JPEG file");
  return bytes;
}

}  // namespace eos::runtime::init

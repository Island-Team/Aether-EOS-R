#include "src/eos/runtime/actionlist/journal.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace eos::runtime::actionlist {
namespace {

constexpr std::array<unsigned char, 4> kMagic{'E', 'O', 'W', 'J'};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::size_t kHeaderBytes = 16;

void Put32(unsigned char* output, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8)
    *output++ = static_cast<unsigned char>((value >> shift) & 0xffU);
}

std::uint32_t Get32(const unsigned char* input) {
  std::uint32_t value = 0;
  for (unsigned shift = 0; shift < 32; shift += 8)
    value |= static_cast<std::uint32_t>(*input++) << shift;
  return value;
}

std::uint32_t Crc32(std::string_view input) {
  std::uint32_t crc = 0xffffffffU;
  for (unsigned char byte : input) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}

[[noreturn]] void IoError(const std::filesystem::path& path, const std::string& operation) {
  throw std::runtime_error("cannot " + operation + " ActionList journal '" + path.string() +
                           "': " + std::strerror(errno));
}

void WriteAll(int descriptor, const void* data, std::size_t size,
              const std::filesystem::path& path) {
  const auto* bytes = static_cast<const unsigned char*>(data);
  std::size_t offset = 0;
  while (offset < size) {
    const ssize_t written = ::write(descriptor, bytes + offset, size - offset);
    if (written <= 0) IoError(path, "write");
    offset += static_cast<std::size_t>(written);
  }
}

void SyncDirectory(const std::filesystem::path& path) {
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (descriptor < 0) IoError(path, "open");
  const bool synced = ::fsync(descriptor) == 0;
  const int close_result = ::close(descriptor);
  if (!synced || close_result != 0) IoError(path, "sync");
}

}  // namespace

Journal::Journal(std::filesystem::path directory) : Journal(std::move(directory), Limits{}) {}

Journal::Journal(std::filesystem::path directory, Limits limits)
    : directory_(std::move(directory)), path_(directory_ / "workflow.journal"), limits_(limits) {
  if (directory_.empty()) throw std::invalid_argument("ActionList journal directory is required");
  if (limits_.maximum_record_bytes == 0 ||
      limits_.maximum_record_bytes > std::numeric_limits<std::uint32_t>::max() ||
      limits_.maximum_journal_bytes < kHeaderBytes + limits_.maximum_record_bytes)
    throw std::invalid_argument("invalid ActionList journal limits");

  std::error_code error;
  const bool created = std::filesystem::create_directories(directory_, error);
  if (error) {
    throw std::runtime_error("cannot create ActionList journal directory '" + directory_.string() +
                             "': " + error.message());
  }
  if (::chmod(directory_.c_str(), S_IRWXU) != 0) IoError(directory_, "secure");
  if (created) SyncDirectory(directory_);
}

void Journal::Append(const std::string& kind, const nlohmann::json& payload) {
  if (kind.empty()) throw std::invalid_argument("ActionList journal record kind is required");
  const std::string encoded =
      nlohmann::json{{"version", kFormatVersion}, {"kind", kind}, {"payload", payload}}.dump();
  if (encoded.size() > limits_.maximum_record_bytes)
    throw std::length_error("ActionList journal record exceeds configured bound");

  std::error_code error;
  const std::uint64_t current = std::filesystem::exists(path_, error)
                                    ? std::filesystem::file_size(path_, error)
                                    : 0;
  if (error) {
    throw std::runtime_error("cannot inspect ActionList journal '" + path_.string() + "': " +
                             error.message());
  }
  if (current > limits_.maximum_journal_bytes - kHeaderBytes - encoded.size())
    throw std::length_error("ActionList journal exceeds configured bound");

  std::array<unsigned char, kHeaderBytes> header{};
  std::copy(kMagic.begin(), kMagic.end(), header.begin());
  Put32(header.data() + 4, kFormatVersion);
  Put32(header.data() + 8, static_cast<std::uint32_t>(encoded.size()));
  Put32(header.data() + 12, Crc32(encoded));
  const int descriptor = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW,
                                S_IRUSR | S_IWUSR);
  if (descriptor < 0) IoError(path_, "open");
  try {
    if (::fchmod(descriptor, S_IRUSR | S_IWUSR) != 0) IoError(path_, "secure");
    WriteAll(descriptor, header.data(), header.size(), path_);
    WriteAll(descriptor, encoded.data(), encoded.size(), path_);
    if (::fsync(descriptor) != 0) IoError(path_, "sync");
  } catch (...) {
    ::close(descriptor);
    throw;
  }
  if (::close(descriptor) != 0) IoError(path_, "close");
  SyncDirectory(directory_);
}

void Journal::Rewrite(const std::vector<nlohmann::json>& records) {
  const auto temporary = directory_ / "workflow.journal.tmp";
  const int descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC |
                                                        O_NOFOLLOW,
                                S_IRUSR | S_IWUSR);
  if (descriptor < 0) IoError(temporary, "open");
  std::uint64_t total = 0;
  try {
    for (const auto& record : records) {
      if (!record.is_object() || !record.contains("kind") || !record.at("kind").is_string() ||
          !record.contains("payload"))
        throw std::invalid_argument("invalid ActionList journal rewrite record");
      const std::string encoded =
          nlohmann::json{{"version", kFormatVersion},
                         {"kind", record.at("kind")},
                         {"payload", record.at("payload")}}
              .dump();
      if (encoded.size() > limits_.maximum_record_bytes)
        throw std::length_error("ActionList journal record exceeds configured bound");
      total += kHeaderBytes + encoded.size();
      if (total > limits_.maximum_journal_bytes)
        throw std::length_error("ActionList compacted journal exceeds configured bound");
      std::array<unsigned char, kHeaderBytes> header{};
      std::copy(kMagic.begin(), kMagic.end(), header.begin());
      Put32(header.data() + 4, kFormatVersion);
      Put32(header.data() + 8, static_cast<std::uint32_t>(encoded.size()));
      Put32(header.data() + 12, Crc32(encoded));
      WriteAll(descriptor, header.data(), header.size(), temporary);
      WriteAll(descriptor, encoded.data(), encoded.size(), temporary);
    }
    if (::fsync(descriptor) != 0) IoError(temporary, "sync");
  } catch (...) {
    ::close(descriptor);
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw;
  }
  if (::close(descriptor) != 0) IoError(temporary, "close");
  std::error_code error;
  std::filesystem::rename(temporary, path_, error);
  if (error) {
    std::filesystem::remove(temporary, error);
    throw std::runtime_error("cannot replace ActionList journal '" + path_.string() + "'");
  }
  SyncDirectory(directory_);
}

std::vector<nlohmann::json> Journal::Replay() {
  std::error_code error;
  if (!std::filesystem::exists(path_, error)) return {};
  if (error) {
    throw std::runtime_error("cannot inspect ActionList journal '" + path_.string() + "': " +
                             error.message());
  }
  const std::uint64_t size = std::filesystem::file_size(path_, error);
  if (error || size > limits_.maximum_journal_bytes)
    throw std::runtime_error("ActionList journal is unreadable or exceeds configured bound");

  std::ifstream input(path_, std::ios::binary);
  if (!input) throw std::runtime_error("cannot read ActionList journal '" + path_.string() + "'");
  std::vector<nlohmann::json> records;
  std::uint64_t offset = 0;
  while (offset < size) {
    const std::uint64_t record_offset = offset;
    if (size - offset < kHeaderBytes) {
      offset = record_offset;
      break;
    }
    std::array<unsigned char, kHeaderBytes> header{};
    input.read(reinterpret_cast<char*>(header.data()), header.size());
    if (!input) throw std::runtime_error("cannot read ActionList journal header");
    offset += header.size();
    if (!std::equal(kMagic.begin(), kMagic.end(), header.begin()) ||
        Get32(header.data() + 4) != kFormatVersion)
      throw std::runtime_error("invalid ActionList journal header at byte " +
                               std::to_string(record_offset));
    const std::uint32_t payload_size = Get32(header.data() + 8);
    if (payload_size > limits_.maximum_record_bytes)
      throw std::runtime_error("oversized ActionList journal record at byte " +
                               std::to_string(record_offset));
    if (size - offset < payload_size) {
      offset = record_offset;
      break;
    }
    std::string encoded(payload_size, '\0');
    input.read(encoded.data(), static_cast<std::streamsize>(encoded.size()));
    if (!input) throw std::runtime_error("cannot read ActionList journal record");
    offset += payload_size;
    if (Crc32(encoded) != Get32(header.data() + 12)) {
      if (offset != size)
        throw std::runtime_error("corrupt ActionList journal record at byte " +
                                 std::to_string(record_offset));
      offset = record_offset;
      break;
    }
    nlohmann::json record;
    try {
      record = nlohmann::json::parse(encoded);
    } catch (const std::exception& exception) {
      throw std::runtime_error("invalid ActionList journal JSON at byte " +
                               std::to_string(record_offset) + ": " + exception.what());
    }
    if (!record.is_object() || record.value("version", 0U) != kFormatVersion ||
        !record.contains("kind") || !record.at("kind").is_string() ||
        !record.contains("payload"))
      throw std::runtime_error("invalid ActionList journal envelope at byte " +
                               std::to_string(record_offset));
    records.push_back(std::move(record));
  }
  input.close();
  if (offset != size) {
    std::filesystem::resize_file(path_, offset, error);
    if (error) {
      throw std::runtime_error("cannot repair ActionList journal tail '" + path_.string() +
                               "': " + error.message());
    }
    SyncDirectory(directory_);
  }
  return records;
}

}  // namespace eos::runtime::actionlist

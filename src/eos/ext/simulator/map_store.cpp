#include "map_store.h"

#include <fcntl.h>
#include <linux/fs.h>
#include <png.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <unordered_set>

#include "src/eos/ext/simulator/raster_map.h"
#include "src/eos/ext/simulator/scene.h"
namespace eos::ext::simulator {
namespace {
using Json = nlohmann::json;
constexpr std::size_t kMaxMediaBytes = 32U * 1024U * 1024U;
void RequireKeys(const Json& v, std::initializer_list<const char*> keys, const char* context) {
  if (!v.is_object()) throw std::invalid_argument(std::string(context) + " must be an object");
  for (auto item = v.begin(); item != v.end(); ++item) {
    bool ok = false;
    for (auto key : keys) ok = ok || item.key() == key;
    if (!ok) throw std::invalid_argument(std::string(context) + " has unknown key: " + item.key());
  }
}
void SyncFile(const std::filesystem::path& path) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) throw std::runtime_error("cannot open saved file for fsync");
  if (::fsync(fd) != 0) {
    ::close(fd);
    throw std::runtime_error("cannot fsync saved file");
  }
  ::close(fd);
}
void AtomicWrite(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
  std::filesystem::create_directories(path.parent_path());
  auto temporary = path.parent_path() / ("." + path.filename().string() + ".tmp");
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out ||
        (!bytes.empty() && !out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size())))
      throw std::runtime_error("cannot write temporary simulator file");
  }
  try {
    SyncFile(temporary);
    std::filesystem::rename(temporary, path);
    int dir = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir >= 0) {
      ::fsync(dir);
      ::close(dir);
    }
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw;
  }
}
void AtomicJson(const std::filesystem::path& path, const Json& value) {
  auto text = value.dump(2) + "\n";
  AtomicWrite(path, {text.begin(), text.end()});
}
void AtomicExchange(const std::filesystem::path& first, const std::filesystem::path& second) {
#if defined(SYS_renameat2) && defined(RENAME_EXCHANGE)
  if (::syscall(SYS_renameat2, AT_FDCWD, first.c_str(), AT_FDCWD, second.c_str(),
                RENAME_EXCHANGE) != 0)
    throw std::runtime_error("cannot atomically exchange map directories");
#else
  (void)first;
  (void)second;
  throw std::runtime_error("atomic map exchange is unsupported on this platform");
#endif
}
std::vector<std::uint8_t> ReadBytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::out_of_range("resource does not exist");
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
Json ReadJson(const std::filesystem::path& path, const char* context) {
  std::ifstream in(path);
  if (!in) throw std::out_of_range(std::string(context) + " does not exist");
  try {
    Json value;
    in >> value;
    return value;
  } catch (const Json::exception& e) {
    throw std::runtime_error(std::string("invalid stored ") + context + ": " + e.what());
  }
}
std::vector<std::uint8_t> WhitePng(std::uint32_t width, std::uint32_t height) {
  png_image image{};
  image.version = PNG_IMAGE_VERSION;
  image.width = width;
  image.height = height;
  image.format = PNG_FORMAT_GRAY;
  std::vector<png_byte> pixels(static_cast<std::size_t>(width) * height, 255);
  png_alloc_size_t size = 0;
  if (!png_image_write_to_memory(&image, nullptr, &size, 0, pixels.data(), 0, nullptr))
    throw std::runtime_error("cannot size raster PNG");
  std::vector<std::uint8_t> result(size);
  if (!png_image_write_to_memory(&image, result.data(), &size, 0, pixels.data(), 0, nullptr))
    throw std::runtime_error("cannot encode raster PNG");
  result.resize(size);
  return result;
}
Json CanonicalMap(const Json& input, const std::string& required_id = {}) {
  RequireKeys(input, {"id", "version", "revision", "raster", "initial_base_pose", "lines"},
              "map manifest");
  Json out = input;
  const auto id = out.at("id").get<std::string>();
  MapStore::ValidateId(id);
  if (!required_id.empty() && id != required_id)
    throw std::invalid_argument("manifest id must match map id");
  if (out.at("version").get<unsigned>() != 2)
    throw std::invalid_argument("unsupported map version");
  (void)out.at("revision").get<std::uint64_t>();
  auto& raster = out.at("raster");
  RequireKeys(raster, {"path", "width_px", "height_px", "meters_per_pixel", "origin"},
              "map raster");
  if (raster.at("path") != "raster.png")
    throw std::invalid_argument("raster.path must be raster.png");
  auto w = raster.at("width_px").get<std::uint32_t>(),
       h = raster.at("height_px").get<std::uint32_t>();
  if (!w || !h || static_cast<std::uint64_t>(w) * h > 100000000ULL)
    throw std::invalid_argument("invalid raster dimensions");
  auto finite = [](const Json& v, std::initializer_list<const char*> keys, const char* name) {
    RequireKeys(v, keys, name);
    for (auto key : keys)
      if (!std::isfinite(v.at(key).get<double>()))
        throw std::invalid_argument(std::string(name) + " must be finite");
  };
  if (!std::isfinite(raster.at("meters_per_pixel").get<double>()) ||
      raster.at("meters_per_pixel").get<double>() <= 0)
    throw std::invalid_argument("invalid meters_per_pixel");
  finite(raster.at("origin"), {"x_m", "y_m"}, "raster.origin");
  finite(out.at("initial_base_pose"), {"x_m", "y_m", "yaw_rad"}, "initial_base_pose");
  (void)ParseScene({{"version", 2}, {"revision", out.at("revision")}, {"lines", out.at("lines")}});
  return out;
}
std::pair<std::string, std::string> Format(const std::string& type) {
  if (type == "image/jpeg") return {".jpg", type};
  if (type == "image/png") return {".png", type};
  if (type == "image/webp") return {".webp", type};
  if (type == "video/mp4") return {".mp4", type};
  if (type == "video/webm") return {".webm", type};
  throw std::invalid_argument("unsupported source media type");
}
}  // namespace
MapStore::MapStore(std::filesystem::path state) : root_(std::move(state) / "simulator/maps") {
  std::filesystem::create_directories(root_);
  root_ = std::filesystem::weakly_canonical(root_);
  library_index_ = root_.parent_path() / "library/index.json";
}
void MapStore::ValidateId(const std::string& id) {
  static const std::regex pattern("^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$");
  if (!std::regex_match(id, pattern)) throw std::invalid_argument("id is invalid");
}
std::filesystem::path MapStore::Bundle(const std::string& id) const {
  ValidateId(id);
  return root_ / id;
}
Json MapStore::ReadMetadata(const std::string& id) const {
  return CanonicalMap(ReadJson(Bundle(id) / "manifest.json", "map"), id);
}
std::vector<std::uint8_t> MapStore::ReadRaster(const std::string& id) const {
  return ReadBytes(Bundle(id) / "raster.png");
}
Json MapStore::List() const {
  Json maps = Json::array();
  for (const auto& e : std::filesystem::directory_iterator(root_))
    if (e.is_directory()) try {
        maps.push_back(ReadMetadata(e.path().filename()));
      } catch (...) {
      }
  std::sort(maps.begin(), maps.end(), [](auto& a, auto& b) { return a.at("id") < b.at("id"); });
  return {{"maps", maps}};
}
void MapStore::ValidateSources(const Json& metadata) const {
  const Json index = ReadJson(library_index_, "source library");
  std::unordered_set<std::string> ids;
  for (const auto& source : index.at("sources")) ids.insert(source.at("id").get<std::string>());
  for (const auto& line : metadata.at("lines"))
    if (ids.find(line.at("source_id").get<std::string>()) == ids.end())
      throw std::invalid_argument("line references an unknown source");
}
Json MapStore::Create(const Json& requested) {
  Json metadata = CanonicalMap(requested);
  if (!metadata.at("lines").empty()) ValidateSources(metadata);
  auto bundle = Bundle(metadata.at("id"));
  if (!std::filesystem::create_directory(bundle)) throw std::invalid_argument("map already exists");
  try {
    AtomicJson(bundle / "manifest.json", metadata);
    AtomicWrite(bundle / "raster.png",
                WhitePng(metadata["raster"]["width_px"], metadata["raster"]["height_px"]));
    (void)RasterMap::LoadBundle(bundle);
  } catch (...) {
    std::filesystem::remove_all(bundle);
    throw;
  }
  return metadata;
}
Json MapStore::ReplaceBundle(const std::string& id, const Json& requested,
                             const std::vector<std::uint8_t>& png, std::uint64_t revision) {
  auto current = ReadMetadata(id);
  if (current.at("revision") != revision) throw std::domain_error("map revision conflict");
  Json metadata = CanonicalMap(requested, id);
  if (metadata.at("revision") != revision) throw std::domain_error("map revision conflict");
  metadata["revision"] = revision + 1;
  ValidateSources(metadata);
  auto bundle = Bundle(id);
  auto stage = root_ / ("." + id + ".stage");
  std::filesystem::remove_all(stage);
  std::filesystem::create_directory(stage);
  try {
    AtomicJson(stage / "manifest.json", metadata);
    AtomicWrite(stage / "raster.png", png);
    (void)RasterMap::LoadBundle(stage);
    AtomicExchange(bundle, stage);
    std::filesystem::remove_all(stage);
  } catch (...) {
    std::filesystem::remove_all(stage);
    throw;
  }
  return metadata;
}
Json MapStore::ReplaceMetadata(const std::string& id, const Json& metadata,
                               std::uint64_t revision) {
  return ReplaceBundle(id, metadata, ReadRaster(id), revision);
}
Json MapStore::ReplaceRaster(const std::string& id, const std::vector<std::uint8_t>& png,
                             std::uint64_t revision) {
  return ReplaceBundle(id, ReadMetadata(id), png, revision);
}
void MapStore::Remove(const std::string& id, std::uint64_t revision) {
  if (ReadMetadata(id).at("revision") != revision) throw std::domain_error("map revision conflict");
  std::filesystem::remove_all(Bundle(id));
}

SourceLibrary::SourceLibrary(std::filesystem::path state)
    : root_(std::move(state) / "simulator/library"), maps_root_(root_.parent_path() / "maps") {
  std::filesystem::create_directories(root_ / "files");
  if (!std::filesystem::exists(root_ / "index.json"))
    AtomicJson(root_ / "index.json", {{"version", 2}, {"revision", 0}, {"sources", Json::array()}});
  root_ = std::filesystem::weakly_canonical(root_);
}
Json SourceLibrary::Index() const {
  Json value = ReadJson(root_ / "index.json", "source library");
  RequireKeys(value, {"version", "revision", "sources"}, "source library");
  if (value.at("version") != 2 || !value.at("sources").is_array())
    throw std::runtime_error("invalid source library index");
  return value;
}
void SourceLibrary::WriteIndex(const Json& value) const { AtomicJson(root_ / "index.json", value); }
Json SourceLibrary::List() const { return Index(); }
Json SourceLibrary::ReadMetadata(const std::string& id) const {
  MapStore::ValidateId(id);
  const auto index = Index();
  for (const auto& source : index.at("sources"))
    if (source.at("id") == id) return source;
  throw std::out_of_range("source does not exist");
}
std::filesystem::path SourceLibrary::Resolve(const std::string& id) const {
  auto source = ReadMetadata(id);
  auto path = (root_ / source.at("path").get<std::string>()).lexically_normal();
  if (path.parent_path() != root_ / "files" || !std::filesystem::is_regular_file(path))
    throw std::runtime_error("source file is missing");
  return path;
}
std::pair<std::string, std::vector<std::uint8_t>> SourceLibrary::Read(const std::string& id) const {
  auto source = ReadMetadata(id);
  return {source.at("media_type"), ReadBytes(Resolve(id))};
}
Json SourceLibrary::Write(const std::string& id, const std::string& type,
                          const std::vector<std::uint8_t>& bytes, std::uint64_t revision) {
  MapStore::ValidateId(id);
  if (bytes.empty() || bytes.size() > kMaxMediaBytes)
    throw std::invalid_argument("source must contain 1 byte through 32 MiB");
  auto index = Index();
  if (index.at("revision") != revision) throw std::domain_error("library revision conflict");
  auto [extension, canonical] = Format(type);
  auto relative = std::filesystem::path("files") / (id + extension);
  AtomicWrite(root_ / relative, bytes);
  for (auto old : {".jpg", ".png", ".webp", ".mp4", ".webm"})
    if (old != extension) {
      std::error_code ignored;
      std::filesystem::remove(root_ / "files" / (id + old), ignored);
    }
  Json metadata{{"id", id},
                {"path", relative.generic_string()},
                {"media_type", canonical},
                {"size_bytes", bytes.size()}};
  auto& sources = index.at("sources");
  auto found =
      std::find_if(sources.begin(), sources.end(), [&](const Json& s) { return s.at("id") == id; });
  if (found == sources.end())
    sources.push_back(metadata);
  else
    *found = metadata;
  index["revision"] = revision + 1;
  WriteIndex(index);
  return metadata;
}
bool SourceLibrary::Referenced(const std::string& id) const {
  if (!std::filesystem::exists(maps_root_)) return false;
  for (const auto& e : std::filesystem::directory_iterator(maps_root_))
    if (e.is_directory()) try {
        auto map = ReadJson(e.path() / "manifest.json", "map");
        for (const auto& line : map.at("lines"))
          if (line.at("source_id") == id) return true;
      } catch (...) {
      }
  return false;
}
void SourceLibrary::Remove(const std::string& id, std::uint64_t revision) {
  if (Referenced(id)) throw std::invalid_argument("cannot remove a source referenced by a map");
  auto index = Index();
  if (index.at("revision") != revision) throw std::domain_error("library revision conflict");
  auto& sources = index.at("sources");
  auto found =
      std::find_if(sources.begin(), sources.end(), [&](const Json& s) { return s.at("id") == id; });
  if (found == sources.end()) throw std::out_of_range("source does not exist");
  auto path = Resolve(id);
  sources.erase(found);
  index["revision"] = revision + 1;
  WriteIndex(index);
  std::filesystem::remove(path);
}
}  // namespace eos::ext::simulator

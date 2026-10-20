/// Native gRPC northbound transport.
#include "eos/internal/northbound/server.h"

#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/health_check_service_interface.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <utility>

#include "src/eos/lib/log.h"
#include "src/eos/runtime/actionlist/manager.h"
#include "src/eos/runtime/northbound/service.h"
#include "src/eos/runtime/northbound/skill_service.h"
#include "src/eos/runtime/northbound/workflow/service.h"

namespace eos::internal {
namespace {

constexpr std::uint64_t kMaxGatewayMessageBytes = 64ULL * 1024ULL * 1024ULL;

std::runtime_error SocketError(const std::filesystem::path& path, const std::string& action) {
  return std::runtime_error("gRPC Unix socket " + action + " failed for '" + path.string() +
                            "': " + std::strerror(errno));
}

void ValidateSocketParent(const std::filesystem::path& path) {
  if (!path.is_absolute() || path.filename().empty() || path != path.lexically_normal()) {
    throw std::runtime_error("gRPC Unix socket path must be absolute and normalized: " +
                             path.string());
  }
  if (path.string().size() > 107) {
    throw std::runtime_error("gRPC Unix socket path exceeds the platform limit: " +
                             path.string());
  }
  std::filesystem::path current = path.root_path();
  for (const auto& component : path.parent_path().relative_path()) {
    current /= component;
    std::error_code error;
    const auto status = std::filesystem::symlink_status(current, error);
    if (error || !std::filesystem::is_directory(status)) {
      throw std::runtime_error("gRPC Unix socket parent is not an existing directory: " +
                               current.string());
    }
    if (std::filesystem::is_symlink(status)) {
      throw std::runtime_error("gRPC Unix socket parent must not contain symlinks: " +
                               current.string());
    }
  }
  struct stat parent_status {};
  if (::stat(path.parent_path().c_str(), &parent_status) != 0) {
    throw SocketError(path.parent_path(), "inspect parent");
  }
  if (parent_status.st_uid != ::geteuid() || (parent_status.st_mode & 0777) != 0700) {
    throw std::runtime_error(
        "gRPC Unix socket parent must be process-owned with mode 0700: " +
        path.parent_path().string());
  }
}

void RemoveStaleSocket(const std::filesystem::path& path) {
  struct stat status {};
  if (::lstat(path.c_str(), &status) != 0) {
    if (errno == ENOENT) return;
    throw SocketError(path, "inspect");
  }
  if (!S_ISSOCK(status.st_mode)) {
    throw std::runtime_error("gRPC Unix socket path exists and is not a socket: " + path.string());
  }

  const int probe = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (probe < 0) throw SocketError(path, "create liveness probe");
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.c_str(), path.string().size() + 1);
  if (::connect(probe, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
    ::close(probe);
    throw std::runtime_error("gRPC Unix socket already has an active listener: " + path.string());
  }
  const int connect_error = errno;
  ::close(probe);
  if (connect_error == ENOENT) return;
  if (connect_error != ECONNREFUSED) {
    errno = connect_error;
    throw SocketError(path, "probe existing socket");
  }

  struct stat confirmed {};
  if (::lstat(path.c_str(), &confirmed) != 0) {
    if (errno == ENOENT) return;
    throw SocketError(path, "confirm stale socket");
  }
  if (!S_ISSOCK(confirmed.st_mode) || confirmed.st_dev != status.st_dev ||
      confirmed.st_ino != status.st_ino) {
    throw std::runtime_error("gRPC Unix socket changed during stale-socket probe: " +
                             path.string());
  }
  if (::unlink(path.c_str()) != 0) throw SocketError(path, "remove stale socket");
}

std::shared_ptr<spdlog::logger>& NorthboundLogger() {
  static auto logger = eos::lib::GetLogger("northbound");
  return logger;
}

}  // namespace

struct NorthboundServer::Impl {
  std::string host;
  std::atomic<std::uint16_t> grpc_port;
  NorthboundUdsOptions uds;
  dev_t uds_device{};
  ino_t uds_inode{};
  bool owns_uds{false};
  std::atomic<bool> ready{false};
  std::atomic<bool> stopping{false};
  std::mutex tick_mutex;
  std::condition_variable tick_wakeup;
  bool stop_tick{false};
  std::thread tick_thread;
  eos::runtime::northbound::NorthboundCore core;
  eos::runtime::northbound::NorthboundGrpcService grpc_service;
  std::unique_ptr<eos::runtime::northbound::SkillGrpcService> skill_grpc_service;
  std::unique_ptr<eos::runtime::northbound::WorkflowGrpcService> workflow_grpc_service;
  std::unique_ptr<grpc::Server> grpc_server;

  Impl(std::string h, std::uint16_t gp,
       const eos::runtime::northbound::NorthboundServices& services, std::function<void()> tick,
       eos::runtime::skills::SkillManager* skill_service,
       eos::runtime::actionlist::Manager* action_lists,
       const std::filesystem::path& workflow_templates, NorthboundUdsOptions uds_options)
      : host(std::move(h)),
        grpc_port(gp),
        uds(std::move(uds_options)),
        core(services, std::move(tick)),
        grpc_service(core) {
    if (skill_service != nullptr) {
      skill_grpc_service =
          std::make_unique<eos::runtime::northbound::SkillGrpcService>(*skill_service);
    }
    if (action_lists != nullptr)
      workflow_grpc_service =
          std::make_unique<eos::runtime::northbound::WorkflowGrpcService>(*action_lists,
                                                                          workflow_templates);
  }

  void StartTick() {
    std::lock_guard<std::mutex> start_lock(tick_mutex);
    if (stop_tick) return;
    tick_thread = std::thread([this] {
      std::unique_lock<std::mutex> lock(tick_mutex);
      while (!tick_wakeup.wait_for(lock, std::chrono::milliseconds(100),
                                   [this] { return stop_tick; })) {
        lock.unlock();
        core.Tick();
        lock.lock();
      }
    });
  }

  void StopTick() {
    {
      std::lock_guard<std::mutex> lock(tick_mutex);
      stop_tick = true;
    }
    tick_wakeup.notify_all();
    if (tick_thread.joinable()) tick_thread.join();
  }

  void RecordUdsIdentity() {
    if (uds.path.empty()) return;
    struct stat status {};
    if (::lstat(uds.path.c_str(), &status) != 0) throw SocketError(uds.path, "inspect bound socket");
    if (!S_ISSOCK(status.st_mode)) {
      throw std::runtime_error("gRPC did not create the configured Unix socket: " +
                               uds.path.string());
    }
    uds_device = status.st_dev;
    uds_inode = status.st_ino;
    owns_uds = true;
  }

  void CleanupUds() noexcept {
    if (!owns_uds) return;
    struct stat status {};
    if (::lstat(uds.path.c_str(), &status) == 0 && S_ISSOCK(status.st_mode) &&
        status.st_dev == uds_device && status.st_ino == uds_inode) {
      (void)::unlink(uds.path.c_str());
    }
    owns_uds = false;
  }
};

NorthboundServer::NorthboundServer(std::string host, std::uint16_t grpc_port,
                                   const eos::runtime::northbound::NorthboundServices& services,
                                   std::function<void()> tick,
                                   eos::runtime::skills::SkillManager* skill_service,
                                   eos::runtime::actionlist::Manager* action_lists,
                                   std::filesystem::path workflow_templates,
                                   NorthboundUdsOptions uds)
    : impl_(new Impl(std::move(host), grpc_port, services, std::move(tick), skill_service,
                     action_lists, workflow_templates, std::move(uds))) {}

NorthboundServer::~NorthboundServer() {
  impl_->StopTick();
  impl_->CleanupUds();
  delete impl_;
}

void NorthboundServer::Run() {
  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
  grpc::ServerBuilder builder;
  builder.SetMaxReceiveMessageSize(static_cast<int>(kMaxGatewayMessageBytes));
  const std::string address = impl_->host + ":" + std::to_string(impl_->grpc_port.load());
  int selected_port = 0;
  builder.AddListeningPort(address, grpc::InsecureServerCredentials(), &selected_port);
  if (!impl_->uds.path.empty()) {
    ValidateSocketParent(impl_->uds.path);
    RemoveStaleSocket(impl_->uds.path);
    builder.AddListeningPort("unix:" + impl_->uds.path.string(),
                             grpc::InsecureServerCredentials());
  }
  builder.RegisterService(&impl_->grpc_service);
  if (impl_->skill_grpc_service) builder.RegisterService(impl_->skill_grpc_service.get());
  if (impl_->workflow_grpc_service) builder.RegisterService(impl_->workflow_grpc_service.get());
  impl_->grpc_server = builder.BuildAndStart();
  if (!impl_->grpc_server) {
    try {
      impl_->RecordUdsIdentity();
      impl_->CleanupUds();
    } catch (...) {
    }
    eos::lib::LogEvent(
        NorthboundLogger(), spdlog::level::err, "grpc_listener_error",
        {{"operation", "bind"}, {"host", impl_->host}, {"port", impl_->grpc_port.load()}});
    throw std::runtime_error("gRPC gateway bind failed: " + address);
  }

  try {
    impl_->RecordUdsIdentity();
    if (!impl_->uds.path.empty() &&
        ::chmod(impl_->uds.path.c_str(), static_cast<mode_t>(impl_->uds.mode)) != 0) {
      throw SocketError(impl_->uds.path, "set permissions");
    }
  } catch (...) {
    impl_->grpc_server->Shutdown();
    impl_->CleanupUds();
    throw;
  }

  impl_->grpc_port.store(static_cast<std::uint16_t>(selected_port));
  impl_->core.Tick();
  impl_->StartTick();
  impl_->ready.store(true);
  eos::lib::LogEvent(NorthboundLogger(), spdlog::level::info, "grpc_listener_started",
                      {{"host", impl_->host},
                       {"port", impl_->grpc_port.load()},
                       {"uds_path", impl_->uds.path.string()}});
  eos::lib::LogEvent(NorthboundLogger(), spdlog::level::info, "gateway_transport_started",
                     {{"transport", "grpc"}});

  if (impl_->stopping.load()) impl_->grpc_server->Shutdown();
  impl_->grpc_server->Wait();
  impl_->ready.store(false);
  impl_->StopTick();
  impl_->CleanupUds();
  eos::lib::LogEvent(NorthboundLogger(), spdlog::level::info, "grpc_listener_stopped", {});
  eos::lib::LogEvent(NorthboundLogger(), spdlog::level::info, "gateway_transport_stopped",
                     {{"transport", "grpc"}});
}

void NorthboundServer::Stop() {
  if (impl_->stopping.exchange(true)) return;
  eos::lib::LogEvent(NorthboundLogger(), spdlog::level::info, "gateway_transport_stopping",
                     {{"transport", "grpc"}});
  if (impl_->grpc_server) impl_->grpc_server->Shutdown();
}

std::uint16_t NorthboundServer::grpc_port() const { return impl_->grpc_port.load(); }

bool NorthboundServer::ready() const { return impl_->ready.load(); }

}  // namespace eos::internal

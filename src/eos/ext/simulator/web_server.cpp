#include "web_server.h"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <cerrno>
#include <chrono>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <regex>
#include <stdexcept>
#include <thread>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "src/eos/ext/simulator/monitor.h"
#include "src/eos/ext/simulator/world.h"
#include "src/eos/lib/log.h"

namespace eos::ext::simulator {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using Json = nlohmann::json;
namespace {
constexpr std::string_view kPrefix = "/v1/eos/simulator";
constexpr std::size_t kMaxBody = 34U * 1024U * 1024U;
constexpr std::size_t kMaxSessions = 64;
std::mutex g_mutation;

std::string Path(std::string target) { return target.substr(0, target.find('?')); }
std::vector<std::string> Parts(const std::string& target) {
  std::vector<std::string> out;
  std::string path = Path(target);
  std::size_t start = std::string(kPrefix).size();
  while (start < path.size()) {
    if (path[start] == '/') ++start;
    if (start >= path.size()) break;
    auto end = path.find('/', start);
    out.push_back(path.substr(start, end - start));
    if (end == std::string::npos) break;
    start = end;
  }
  return out;
}
std::uint64_t Revision(const std::string& value) {
  if (value.empty()) throw std::length_error("If-Match revision is required");
  std::string text = value;
  if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
    text = text.substr(1, text.size() - 2);
  std::size_t used = 0;
  const auto result = std::stoull(text, &used);
  if (used != text.size()) throw std::invalid_argument("invalid If-Match revision");
  return result;
}
WebResponse J(unsigned status, Json value, std::uint64_t revision = 0) {
  return {status, "application/json", value.dump(), revision ? "\"" + std::to_string(revision) + "\"" : ""};
}
std::pair<Json,std::vector<std::uint8_t>> BundleParts(const std::string& body,const std::string& type){
  std::smatch m;std::regex br(R"(boundary=(?:\"([^\"]+)\"|([^;\s]+)))",std::regex::icase);if(!std::regex_search(type,m,br))throw std::invalid_argument("multipart boundary is required");std::string boundary=m[1].matched?m[1].str():m[2].str(), marker="--"+boundary;Json manifest;std::vector<std::uint8_t> raster;bool hm=false,hr=false;std::size_t at=0;while((at=body.find(marker,at))!=std::string::npos){at+=marker.size();if(body.compare(at,2,"--")==0)break;if(body.compare(at,2,"\r\n")==0)at+=2;auto headers_end=body.find("\r\n\r\n",at);if(headers_end==std::string::npos)throw std::invalid_argument("malformed multipart body");auto next=body.find("\r\n"+marker,headers_end+4);if(next==std::string::npos)throw std::invalid_argument("unterminated multipart body");auto headers=body.substr(at,headers_end-at),data=body.substr(headers_end+4,next-headers_end-4);if(headers.find("name=\"manifest\"")!=std::string::npos){manifest=Json::parse(data);hm=true;}else if(headers.find("name=\"raster\"")!=std::string::npos){raster.assign(data.begin(),data.end());hr=true;}else throw std::invalid_argument("unexpected multipart part");at=next+2;}if(!hm||!hr)throw std::invalid_argument("multipart body requires manifest and raster parts");return {manifest,raster};
}
void AddCors(const http::request<http::string_body>& request,
             http::response<http::string_body>& response) {
  response.set(http::field::vary, "Origin");
  auto origin = request.find(http::field::origin);
  if (origin != request.end() && SimulatorCorsOriginAllowed(std::string(origin->value()))) {
    response.set(http::field::access_control_allow_origin, origin->value());
    response.set(http::field::access_control_allow_methods, "GET, POST, PUT, DELETE, OPTIONS");
    response.set(http::field::access_control_allow_headers, "Content-Type, If-Match");
    response.set(http::field::access_control_expose_headers, "ETag");
  }
}
}  // namespace

bool SimulatorCorsOriginAllowed(const std::string& origin) {
  return origin == "https://os-island.com" || origin == "https://www.os-island.com" ||
         origin == "http://localhost:3000" || origin == "http://127.0.0.1:3000";
}

WebServer::WebServer(MapStore maps, SourceLibrary sources, World& world, std::string active_map,
                     std::function<void()> reload)
    : maps_(std::move(maps)), sources_(std::move(sources)), world_(world),
      active_map_(std::move(active_map)), reload_(std::move(reload)) {}

WebResponse WebServer::Handle(const std::string& method, const std::string& target,
                              const std::string& body, const std::string& content_type,
                              const std::string& if_match) {
  if (Path(target).rfind(std::string(kPrefix), 0) != 0)
    return J(404, {{"error", "route not found"}});
  if (method == "OPTIONS") return {204};
  std::unique_lock lock(g_mutation, std::defer_lock);
  if (method == "POST" || method == "PUT" || method == "DELETE") lock.lock();
  try {
    const auto p = Parts(target);
    if (p.empty()) {
      if (method == "GET") return J(200, {{"version", 2}, {"active_map_id", active_map_}});
      return J(405, {{"error", "method not allowed"}});
    }
    if (p[0] == "snapshot" && p.size() == 1 && method == "GET")
      return J(200, SimulatorJson(world_.SimulatorState()));
    if (p[0] == "raster" && p.size() == 1 && method == "GET") {
      const auto raster = world_.SimulatorRasterPng();
      return {200, "image/png", {reinterpret_cast<const char*>(raster.png.data()), raster.png.size()},
              "\"" + raster.map_id + ":" + std::to_string(raster.revision) + "\""};
    }
    if (p[0] == "maps") {
      if (p.size() == 1) {
        if (method == "GET") return J(200, maps_.List());
        if (method == "POST") {
          auto value = maps_.Create(Json::parse(body));
          ++generation_;
          return J(201, value, value.at("revision"));
        }
      } else {
        const auto& id = p[1];
        if (p.size() == 2 && method == "GET") { auto v=maps_.ReadMetadata(id); return J(200,v,v.at("revision")); }
        if (p.size() == 2 && method == "PUT") {
          auto v=maps_.ReplaceMetadata(id,Json::parse(body),Revision(if_match)); if(id==active_map_) reload_(); ++generation_; return J(200,v,v.at("revision"));
        }
        if (p.size() == 2 && method == "DELETE") {
          if (id == active_map_) throw std::domain_error("active map cannot be deleted");
          maps_.Remove(id, Revision(if_match));
          ++generation_;
          return {204};
        }
        if (p.size() == 3 && p[2] == "raster") {
          if (method == "GET") { auto b=maps_.ReadRaster(id); auto m=maps_.ReadMetadata(id); return {200,"image/png",{reinterpret_cast<const char*>(b.data()),b.size()},"\""+std::to_string(m.at("revision").get<std::uint64_t>())+"\""}; }
          if (method == "PUT") { auto v=maps_.ReplaceRaster(id,{body.begin(),body.end()},Revision(if_match)); if(id==active_map_) reload_(); ++generation_; return J(200,v,v.at("revision")); }
        }
        if (p.size() == 3 && p[2] == "bundle" && method == "PUT") { auto parts=BundleParts(body,content_type); auto v=maps_.ReplaceBundle(id,parts.first,parts.second,Revision(if_match)); if(id==active_map_)reload_(); ++generation_; return J(200,v,v.at("revision")); }
      }
    }
    if (p[0] == "sources") {
      if (p.size() == 1 && method == "GET") return J(200, sources_.List());
      if (p.size() == 2) {
        const auto& id=p[1];
        if(method=="GET") { auto [type,b]=sources_.Read(id); return {200,type,{reinterpret_cast<const char*>(b.data()),b.size()}}; }
        if(method=="PUT") { auto v=sources_.Write(id,content_type,{body.begin(),body.end()},Revision(if_match)); if(!active_map_.empty())reload_(); ++generation_; return J(201,v); }
        if(method=="DELETE") { sources_.Remove(id,Revision(if_match)); ++generation_; return {204}; }
      }
    }
    return J(404, {{"error", "route not found"}});
  } catch (const std::out_of_range& e) { return J(404, {{"error",e.what()}}); }
  catch (const std::length_error& e) { return J(428, {{"error",e.what()}}); }
  catch (const std::domain_error& e) { return J(409, {{"error",e.what()}}); }
  catch (const Json::exception& e) { return J(400, {{"error",std::string("invalid JSON: ")+e.what()}}); }
  catch (const std::invalid_argument& e) { return J(400, {{"error",e.what()}}); }
  catch (const std::exception& e) { return J(500, {{"error","simulator storage failure"},{"detail",e.what()}}); }
}

void WebServer::Serve(const std::string& host, std::uint16_t port, const std::atomic_bool& stop) {
  asio::io_context io;
  tcp::acceptor acceptor(io, {asio::ip::make_address(host), port});
  acceptor.non_blocking(true);
  auto logger=lib::GetLogger("simulator.web");
  struct Session {
    std::thread worker;
    int control;
    std::shared_ptr<std::atomic_bool> done;
  };
  std::vector<Session> sessions;
  sessions.reserve(kMaxSessions);
  const auto reap = [&] {
    for (auto it = sessions.begin(); it != sessions.end();) {
      if (!it->done->load(std::memory_order_acquire)) {
        ++it;
        continue;
      }
      if (it->worker.joinable()) it->worker.join();
      ::close(it->control);
      it = sessions.erase(it);
    }
  };
  const auto shutdown_sessions = [&] {
    boost::system::error_code ignored;
    acceptor.close(ignored);
    for (auto& session : sessions) ::shutdown(session.control, SHUT_RDWR);
    for (auto& session : sessions)
      if (session.worker.joinable()) session.worker.join();
    for (auto& session : sessions) ::close(session.control);
    sessions.clear();
  };
  lib::LogEvent(logger,spdlog::level::info,"simulator_web_started",{{"host",host},{"port",port}});
  try {
    while(!stop.load()) {
      reap();
      if (sessions.size() == kMaxSessions) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      }
      tcp::socket socket(io);
      boost::system::error_code ec;
      acceptor.accept(socket,ec);
      if(ec==asio::error::would_block||ec==asio::error::try_again){std::this_thread::sleep_for(std::chrono::milliseconds(20));continue;}
      if(ec) throw std::system_error(ec);
      const int control = ::fcntl(socket.native_handle(), F_DUPFD_CLOEXEC, 0);
      if (control < 0) continue;
      auto done = std::make_shared<std::atomic_bool>(false);
      std::thread worker;
      try {
        worker = std::thread(
            [this, socket = std::move(socket), done, &stop, logger]() mutable noexcept {
    struct Completion {
      std::shared_ptr<std::atomic_bool> done;
      ~Completion() {
        done->store(true, std::memory_order_release);
      }
    } completion{done};
    try {
    boost::system::error_code ec;
    beast::flat_buffer buffer; http::request_parser<http::string_body> parser; parser.body_limit(kMaxBody);
    http::read(socket,buffer,parser,ec); if(ec) return; auto request=parser.release();
    if(websocket::is_upgrade(request)&&Path(std::string(request.target()))==std::string(kPrefix)+"/ws") {
      const auto origin = request.find(http::field::origin);
      if (origin != request.end() &&
          !SimulatorCorsOriginAllowed(std::string(origin->value())))
        return;
      websocket::stream<tcp::socket&> ws(socket); ws.accept(request,ec); if(ec) return;
      auto snapshot = [&] {
        std::lock_guard mutation_lock(g_mutation);
        auto simulator = SimulatorJson(world_.SimulatorState());
        auto maps = maps_.List();
        auto library = sources_.List();
        std::uint64_t revision = simulator.at("map").at("revision");
        return Json{{"type", "snapshot"},
                    {"version", 1},
                    {"revision", revision},
                    {"payload", {{"simulator", std::move(simulator)},
                                 {"maps", std::move(maps)},
                                 {"library", std::move(library)}}}};
      };
      ws.read_message_max(1024U * 1024U);
      ws.text(true);
      auto snap = snapshot();
      ws.write(asio::buffer(snap.dump()), ec);
      std::uint64_t observed_generation = generation_.load();
      lib::LogEvent(logger, spdlog::level::debug, "simulator_websocket_connected");
      while (!ec && !stop.load()) {
        pollfd descriptor{ws.next_layer().native_handle(), POLLIN, 0};
        const int ready = ::poll(&descriptor, 1, 250);
        if (ready < 0) {
          if (errno == EINTR) continue;
          break;
        }
        if (ready == 0) {
          const std::uint64_t current_generation = generation_.load();
          if (current_generation != observed_generation) {
            auto update = snapshot();
            ws.write(asio::buffer(update.dump()), ec);
            observed_generation = current_generation;
          }
          continue;
        }
        if ((descriptor.revents & POLLIN) == 0) break;
        beast::flat_buffer in;
        ws.read(in, ec);
        if (ec) break;
        try {
          auto msg = Json::parse(beast::buffers_to_string(in.data()));
          Json out;
          auto type = msg.value("type", "");
          if (msg.value("version", 0) != 1)
            out = {{"type", "error"}, {"version", 1},
                   {"payload", {{"code", "unsupported_version"},
                                {"message", "version 1 is required"}}}};
          else if (type == "resync")
            out = snapshot();
          else if (type == "map.create") {
            auto response = Handle("POST", std::string(kPrefix) + "/maps",
                                   msg.at("payload").dump(), "application/json");
            if (response.status >= 400)
              out = {{"type", "error"}, {"version", 1},
                     {"payload", Json::parse(response.body)}};
            else {
              auto saved = Json::parse(response.body);
              out = {{"type", "ack"}, {"version", 1}, {"revision", saved.at("revision")},
                     {"payload", {{"operation", type}, {"map", saved}}}};
            }
          } else if (type == "map.update") {
            auto payload = msg.at("payload");
            auto id = payload.at("id").get<std::string>();
            auto expected = msg.at("expected_revision").get<std::uint64_t>();
            auto response = Handle("PUT", std::string(kPrefix) + "/maps/" + id,
                                   payload.dump(), "application/json",
                                   std::to_string(expected));
            if (response.status >= 400)
              out = {{"type", "error"}, {"version", 1}, {"revision", expected},
                     {"payload", Json::parse(response.body)}};
            else {
              auto saved = Json::parse(response.body);
              out = {{"type", "ack"}, {"version", 1}, {"revision", saved.at("revision")},
                     {"payload", {{"operation", type}, {"map", saved}}}};
            }
          } else if (type == "map.delete") {
            auto payload = msg.at("payload");
            auto expected = msg.at("expected_revision").get<std::uint64_t>();
            auto response = Handle("DELETE", std::string(kPrefix) + "/maps/" +
                                                 payload.at("id").get<std::string>(),
                                   {}, {}, std::to_string(expected));
            if (response.status >= 400)
              out = {{"type", "error"}, {"version", 1}, {"revision", expected},
                     {"payload", Json::parse(response.body)}};
            else
              out = {{"type", "ack"}, {"version", 1}, {"revision", expected},
                     {"payload", {{"operation", type}}}};
          } else
            out = {{"type", "error"}, {"version", 1},
                   {"payload", {{"code", "unsupported_message"},
                                {"message", "unsupported message type"}}}};
          if (msg.contains("request_id")) out["request_id"] = msg["request_id"];
          ws.write(asio::buffer(out.dump()), ec);
        } catch (const std::exception& e) {
          Json out = {{"type", "error"}, {"version", 1},
                      {"payload", {{"code", "invalid_message"}, {"message", e.what()}}}};
          ws.write(asio::buffer(out.dump()), ec);
        }
      }
      lib::LogEvent(logger, spdlog::level::debug, "simulator_websocket_disconnected");
      return;
    }
    auto ct=request.find(http::field::content_type), im=request.find(http::field::if_match);
    auto result=Handle(std::string(request.method_string()),std::string(request.target()),request.body(),ct==request.end()?"":std::string(ct->value()),im==request.end()?"":std::string(im->value()));
    http::response<http::string_body> response{static_cast<http::status>(result.status),request.version()}; response.set(http::field::server,"eos-simulator"); response.set(http::field::content_type,result.content_type); if(!result.etag.empty())response.set(http::field::etag,result.etag); AddCors(request,response); response.body()=std::move(result.body); response.prepare_payload(); http::write(socket,response,ec);
    } catch (const std::exception& error) {
      try {
        lib::LogEvent(logger, spdlog::level::err, "simulator_web_session_failed",
                      {{"reason", error.what()}});
      } catch (...) {
      }
    } catch (...) {
      try {
        lib::LogEvent(logger, spdlog::level::err, "simulator_web_session_failed",
                      {{"reason", "unknown exception"}});
      } catch (...) {
      }
    }
    });
      } catch (...) {
        ::close(control);
        throw;
      }
      sessions.push_back({std::move(worker), control, done});
    }
  } catch (...) {
    shutdown_sessions();
    throw;
  }
  shutdown_sessions();
  lib::LogEvent(logger,spdlog::level::info,"simulator_web_stopped");
}
}  // namespace eos::ext::simulator

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <memory>
#include <poll.h>
#include <system_error>
#include <thread>
#include <vector>

#include "src/eos/ext/simulator/web_server.h"
#include "src/eos/ext/simulator/world.h"

namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

std::uint16_t AvailablePort() {
  asio::io_context io;
  tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
  return acceptor.local_endpoint().port();
}

class WebServerTest : public testing::Test {
 protected:
  WebServerTest()
      : root_(std::filesystem::temp_directory_path() /
              ("eos-simulator-web-test-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))),
        port_(AvailablePort()),
        server_(eos::ext::simulator::MapStore(root_),
                eos::ext::simulator::SourceLibrary(root_), world_, "", [] {}) {}

  ~WebServerTest() override {
    Stop();
    std::filesystem::remove_all(root_);
  }

  void Start() {
    thread_ = std::thread([this] {
      try {
        server_.Serve("127.0.0.1", port_, stop_);
      } catch (...) {
        error_ = std::current_exception();
      }
      exited_.store(true);
    });

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
      asio::io_context io;
      tcp::socket probe(io);
      boost::system::error_code error;
      probe.connect({asio::ip::address_v4::loopback(), port_}, error);
      if (!error) return;
      std::this_thread::sleep_for(10ms);
    }
    FAIL() << "simulator web server did not start";
  }

  void Stop() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    if (error_) ADD_FAILURE() << "simulator web server exited with an exception";
  }

  std::unique_ptr<tcp::socket> Connect() {
    auto socket = std::make_unique<tcp::socket>(io_);
    socket->connect({asio::ip::address_v4::loopback(), port_});
    return socket;
  }

  http::status GetRoot(std::chrono::milliseconds timeout = 2s) {
    tcp::socket stream(io_);
    stream.connect({asio::ip::address_v4::loopback(), port_});
    http::request<http::empty_body> request{http::verb::get, "/v1/eos/simulator", 11};
    request.set(http::field::host, "localhost");
    http::write(stream, request);
    pollfd descriptor{stream.native_handle(), POLLIN, 0};
    const int ready = ::poll(&descriptor, 1, static_cast<int>(timeout.count()));
    if (ready == 0) throw boost::system::system_error(asio::error::timed_out);
    if (ready < 0) throw std::system_error(errno, std::generic_category());
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(stream, buffer, response);
    return response.result();
  }

  std::filesystem::path root_;
  std::uint16_t port_;
  asio::io_context io_;
  eos::ext::simulator::World world_;
  eos::ext::simulator::WebServer server_;
  std::atomic_bool stop_{false};
  std::atomic_bool exited_{false};
  std::thread thread_;
  std::exception_ptr error_;
};

TEST_F(WebServerTest, IncompleteRequestDoesNotBlockAnotherConnection) {
  Start();
  auto stalled = Connect();
  asio::write(*stalled, asio::buffer(std::string("GET /v1/eos/simulator HTTP/1.1\r\n")));

  EXPECT_EQ(GetRoot(), http::status::ok);
}

TEST_F(WebServerTest, SessionLimitQueuesAndReusesCapacity) {
  Start();
  std::vector<std::unique_ptr<tcp::socket>> stalled;
  stalled.reserve(64);
  for (std::size_t i = 0; i < 64; ++i) {
    auto socket = Connect();
    asio::write(*socket, asio::buffer(std::string("GET / HTTP/1.1\r\n")));
    stalled.push_back(std::move(socket));
  }
  std::this_thread::sleep_for(200ms);

  EXPECT_THROW((void)GetRoot(200ms), boost::system::system_error);
  stalled.back()->close();
  stalled.pop_back();
  EXPECT_EQ(GetRoot(), http::status::ok);
}

TEST_F(WebServerTest, ShutdownInterruptsIncompleteRequest) {
  Start();
  auto stalled = Connect();
  asio::write(*stalled, asio::buffer(std::string("GET / HTTP/1.1\r\n")));
  std::this_thread::sleep_for(50ms);

  stop_.store(true);
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!exited_.load() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(10ms);
  EXPECT_TRUE(exited_.load());
  if (!exited_.load()) stalled->close();
}

TEST_F(WebServerTest, ShutdownInterruptsIdleWebSocket) {
  Start();
  websocket::stream<tcp::socket> socket(io_);
  socket.next_layer().connect({asio::ip::address_v4::loopback(), port_});
  socket.handshake("localhost", "/v1/eos/simulator/ws");
  beast::flat_buffer snapshot;
  socket.read(snapshot);

  stop_.store(true);
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!exited_.load() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(10ms);
  EXPECT_TRUE(exited_.load());
  if (!exited_.load()) socket.next_layer().close();
}
}  // namespace

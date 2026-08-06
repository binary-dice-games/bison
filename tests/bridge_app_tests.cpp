// MIT License © 2025 Binary Dice Games
// Tests for src/app/bridge/bridge_app.hpp

#include "src/app/bridge/bridge_app.hpp"
#include "src/rmi/rmi.hpp"
#include "src/rmi/transport/tls_socket_transport.hpp"
#include "tests/tls_test_certs.hpp"

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

// bridge_app.cpp / downstream_transport_flags.cpp / upstream_transport_flags.cpp
// DECLARE these flags for CLI-driven binaries to DEFINE; this test executable
// calls run_with_transport() directly (bypassing run()'s CLI parsing) but
// still links the object code that references them, so they must be defined
// here. Mirrors src/app/abi_flags.cpp's rationale for bison_abi.dll.
DEFINE_string(downstream_transport, "tcp", "test default");
DEFINE_string(downstream_host, "127.0.0.1", "test default");
DEFINE_int32(downstream_port, 7070, "test default");
DEFINE_string(downstream_name, "", "test default");
DEFINE_string(cmd, "", "test default");
DEFINE_int32(timeout, 30000, "test default");
DEFINE_bool(debugger, false, "test default");
DEFINE_string(downstream_cert_file, "", "test default");
DEFINE_string(downstream_cert_pem, "", "test default");
DEFINE_string(downstream_key_file, "", "test default");
DEFINE_string(downstream_key_pem, "", "test default");
DEFINE_string(downstream_key_password, "", "test default");
DEFINE_string(downstream_client_auth, "none", "test default");
DEFINE_string(downstream_ca_file, "", "test default");
DEFINE_string(downstream_ca_pem, "", "test default");
DEFINE_string(upstream_transport, "tcp", "test default");
DEFINE_string(upstream_host, "127.0.0.1", "test default");
DEFINE_int32(upstream_port, 7070, "test default");
DEFINE_string(upstream_name, "", "test default");
DEFINE_string(upstream_server_name, "", "test default");
DEFINE_string(upstream_ca_file, "", "test default");
DEFINE_string(upstream_ca_pem, "", "test default");
DEFINE_bool(upstream_insecure_skip_verify, false, "test default");
DEFINE_string(upstream_cert_file, "", "test default");
DEFINE_string(upstream_cert_pem, "", "test default");
DEFINE_string(upstream_key_file, "", "test default");
DEFINE_string(upstream_key_pem, "", "test default");
DEFINE_string(upstream_key_password, "", "test default");

using namespace bdg::bison;
using namespace bdg::bison::rmi;
using namespace bdg::bison::rmi::shared::constants;
using namespace bdg::bison::rmi::transport;
using namespace bdg::bison::rmi::transport::test;

namespace {

static void clearClassRegistry() {
  dynamic::getRegistry().wlock()->clear();
}

// Blocks run_with_transport() until stop() is called from another thread.
class manual_shutdown_gate {
 public:
  void wait() {
    std::unique_lock<std::mutex> lk(mtx_);
    cv_.wait(lk, [&] { return stopped_; });
  }

  void stop() {
    {
      std::lock_guard<std::mutex> lk(mtx_);
      stopped_ = true;
    }
    cv_.notify_all();
  }

 private:
  std::mutex mtx_;
  std::condition_variable cv_;
  bool stopped_ = false;
};

class test_bridge_app : public app::bridge_app {
 public:
  using app::bridge_app::run_with_transport;

  std::atomic<int> client_connected_count{0};
  std::atomic<int> client_disconnected_count{0};
  manual_shutdown_gate gate;

 protected:
  void on_client_connected(rmi::context& ctx) const override {
    (void)ctx;
    const_cast<test_bridge_app*>(this)->client_connected_count++;
  }

  void on_client_disconnected(rmi::context& ctx) const override {
    (void)ctx;
    const_cast<test_bridge_app*>(this)->client_disconnected_count++;
  }

  void wait_for_shutdown() override {
    gate.wait();
  }
};

class BridgeAppTest : public ::testing::Test {
 protected:
  memory_server_transport upstream_transport_;
  std::unique_ptr<server> upstream_srv_;
  memory_server_transport downstream_transport_;

  void SetUp() override {
    clearClassRegistry();

    auto proto = dynamic_ptr{"Calculator"_key, {}};
    proto->addMethod("add"_key, method{[](dynamic& /*self*/, const dynamic& params) -> dynamic {
                       float a = params["a"_key];
                       float b = params["b"_key];
                       dynamic result;
                       result["result"_key] = a + b;
                       return result;
                     }});
    dynamic::addClass(0U, proto, 0U);

    upstream_srv_ = std::make_unique<server>(upstream_transport_);
    upstream_srv_->listen();
  }

  void TearDown() override {
    upstream_srv_->stop();
    upstream_srv_.reset();
  }
};

} // namespace

TEST_F(BridgeAppTest, RelaysClientCallsAndFiresConnectHooks) {
  test_bridge_app app;

  std::thread bridge_thread([&] {
    app.run_with_transport(
        downstream_transport_, std::make_unique<memory_client_transport>(upstream_transport_.connect()));
  });

  // Give the bridge thread a moment to start().
  std::this_thread::sleep_for(std::chrono::milliseconds{50});

  {
    client c{downstream_transport_.connect()};
    c.connect();

    auto calc = c.instantiate(0U, "Calculator"_key).get();
    EXPECT_TRUE(calc.valid());

    dynamic params;
    params["a"_key] = 10.0f;
    params["b"_key] = 3.0f;
    auto result = calc.call("add"_key, std::move(params)).get();
    float res = result["result"_key];
    EXPECT_FLOAT_EQ(res, 13.0f);

    c.destroy(std::move(calc));
    c.disconnect();
  }

  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  EXPECT_EQ(app.client_connected_count.load(), 1);
  EXPECT_EQ(app.client_disconnected_count.load(), 1);

  app.gate.stop();
  bridge_thread.join();
}

// ─────────────────────────────────────────────────────────────────────────────
// TLS on both hops
// ─────────────────────────────────────────────────────────────────────────────
//
// TLS has no in-memory variant (encryption operates on a real byte stream),
// so unlike BridgeAppTest's memory_*_transport fixture, this exercises real
// loopback sockets on both the downstream and upstream hops. Also exercises
// on_downstream_listen_params()/on_upstream_connect_params()'s TLS-gated
// population, which only fires when FLAGS_downstream_transport/
// FLAGS_upstream_transport are "tls" -- set directly here since this test
// (like BridgeAppTest above) calls run_with_transport() directly, bypassing
// run()'s CLI flag parsing.

TEST(BridgeAppTlsTest, RelaysClientCallsOverTls) {
  clearClassRegistry();

  auto proto = dynamic_ptr{"Calculator"_key, {}};
  proto->addMethod("add"_key, method{[](dynamic& /*self*/, const dynamic& params) -> dynamic {
                     float a = params["a"_key];
                     float b = params["b"_key];
                     dynamic result;
                     result["result"_key] = a + b;
                     return result;
                   }});
  dynamic::addClass(0U, proto, 0U);

  static std::atomic<uint16_t> next_port{29800};
  const auto upstream_port = next_port.fetch_add(1);
  const auto downstream_port = next_port.fetch_add(1);

  // Real upstream TLS server -- what the bridge dials out to.
  tls_socket_server_transport upstream_listener{"127.0.0.1", upstream_port};
  server upstream_srv{upstream_listener};
  upstream_srv.listen(tls_server_params(kTestServerCert, kTestServerKey));

  FLAGS_downstream_transport = "tls";
  FLAGS_downstream_cert_pem = kTestServerCert;
  FLAGS_downstream_key_pem = kTestServerKey;
  FLAGS_upstream_transport = "tls";
  FLAGS_upstream_ca_pem = kTestCaCert;

  tls_socket_server_transport downstream_transport{"127.0.0.1", downstream_port};
  auto upstream_client_transport =
      std::make_unique<tls_socket_client_transport>("127.0.0.1", upstream_port);

  test_bridge_app app;
  std::thread bridge_thread(
      [&] { app.run_with_transport(downstream_transport, std::move(upstream_client_transport)); });

  // Give the bridge thread a moment to start() (including both TLS handshakes).
  std::this_thread::sleep_for(std::chrono::milliseconds{200});

  {
    client c{tls_socket_client_transport{"127.0.0.1", downstream_port}};
    dynamic connect_params;
    connect_params["ca_pem"_key] = kTestCaCert;
    c.connect(std::move(connect_params));

    auto calc = c.instantiate(0U, "Calculator"_key).get();
    EXPECT_TRUE(calc.valid());

    dynamic params;
    params["a"_key] = 10.0f;
    params["b"_key] = 3.0f;
    auto result = calc.call("add"_key, std::move(params)).get();
    float res = result["result"_key];
    EXPECT_FLOAT_EQ(res, 13.0f);

    c.destroy(std::move(calc));
    c.disconnect();
  }

  app.gate.stop();
  bridge_thread.join();
  upstream_srv.stop();

  // Reset flags to their test defaults -- gflags globals persist across
  // TESTs sharing this process, and other tests in this binary assume "tcp".
  FLAGS_downstream_transport = "tcp";
  FLAGS_downstream_cert_pem.clear();
  FLAGS_downstream_key_pem.clear();
  FLAGS_upstream_transport = "tcp";
  FLAGS_upstream_ca_pem.clear();
}

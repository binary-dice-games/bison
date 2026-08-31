// MIT License © 2025 Binary Dice Games
// Tests for src/app/server/server_app.hpp

#include "src/app/server/server_app.hpp"
#include "src/rmi/rmi.hpp"
#include "src/rmi/transport/tls_socket_transport.hpp"
#include "tests/tls_test_certs.hpp"

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

// server_app.cpp / transport_flags.cpp DECLARE these flags for CLI-driven
// binaries to DEFINE; this test executable calls run_with_transport()
// directly (bypassing run()'s CLI parsing) but still links the object code
// that references them, so they must be defined here. Mirrors
// bridge_app_tests.cpp's identical rationale.
DEFINE_string(transport, "tcp", "test default");
DEFINE_string(host, "127.0.0.1", "test default");
DEFINE_int32(port, 7070, "test default");
DEFINE_string(name, "", "test default");
DEFINE_string(cmd, "", "test default");
DEFINE_string(verbose, "none", "test default");
DEFINE_bool(debugger, false, "test default");
DEFINE_string(cert_file, "", "test default");
DEFINE_string(cert_pem, "", "test default");
DEFINE_string(key_file, "", "test default");
DEFINE_string(key_pem, "", "test default");
DEFINE_string(key_password, "", "test default");
DEFINE_string(client_auth, "none", "test default");
DEFINE_string(ca_file, "", "test default");
DEFINE_string(ca_pem, "", "test default");

using namespace bdg::bison;
using namespace bdg::bison::rmi;
using namespace bdg::bison::rmi::transport;
using namespace bdg::bison::rmi::transport::test;

namespace {

void clearClassRegistry() {
  dynamic::getRegistry().wlock()->clear();
}

void registerCalculator() {
  auto proto = dynamic_ptr{"Calculator"_key, {}};
  proto->addMethod("add"_key, method{[](dynamic& /*self*/, const dynamic& params) -> dynamic {
                     float a = params["a"_key];
                     float b = params["b"_key];
                     dynamic result;
                     result["result"_key] = a + b;
                     return result;
                   }});
  dynamic::addClass(0U, proto, 0U);
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

class test_server_app : public app::server_app {
 public:
  using app::server_app::on_listen_params;
  using app::server_app::run_with_transport;

  manual_shutdown_gate gate;

 protected:
  void register_classes() override {
    // Tests register classes directly (clearClassRegistry() + registerCalculator())
    // before constructing this app, matching bridge_app_tests.cpp's style.
  }

  void wait_for_shutdown() override {
    gate.wait();
  }
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// on_listen_params()
// ─────────────────────────────────────────────────────────────────────────────

TEST(ServerAppTest, OnListenParamsIsNoOpForNonTlsTransport) {
  FLAGS_transport = "tcp";
  test_server_app app;
  dynamic params;
  app.on_listen_params(params);
  EXPECT_EQ(params.size(), 0u);
}

TEST(ServerAppTest, OnListenParamsPopulatesTlsFieldsWhenSelected) {
  FLAGS_transport = "tls";
  FLAGS_cert_pem = kTestServerCert;
  FLAGS_key_pem = kTestServerKey;

  test_server_app app;
  dynamic params;
  app.on_listen_params(params);

  EXPECT_EQ(params["cert_pem"_key].as<std::string>(), kTestServerCert);
  EXPECT_EQ(params["key_pem"_key].as<std::string>(), kTestServerKey);
  EXPECT_EQ(params["client_auth"_key].as<std::string>(), "none");

  FLAGS_transport = "tcp";
  FLAGS_cert_pem.clear();
  FLAGS_key_pem.clear();
}

// ─────────────────────────────────────────────────────────────────────────────
// run_with_transport() end-to-end
// ─────────────────────────────────────────────────────────────────────────────

TEST(ServerAppTest, MissingCertAndKeySurfacesErrorInsteadOfSilentlyStarting) {
  FLAGS_transport = "tls";
  FLAGS_cert_pem.clear();
  FLAGS_key_pem.clear();

  static std::atomic<uint16_t> next_port{29900};
  const auto port = next_port.fetch_add(1);

  tls_socket_server_transport tls_transport{"127.0.0.1", port};
  test_server_app app;
  EXPECT_THROW(app.run_with_transport(tls_transport), std::exception);

  FLAGS_transport = "tcp";
}

TEST(ServerAppTest, HandshakeAndRmiCallSucceedOverTls) {
  clearClassRegistry();
  registerCalculator();

  static std::atomic<uint16_t> next_port{29950};
  const auto port = next_port.fetch_add(1);

  FLAGS_transport = "tls";
  FLAGS_cert_pem = kTestServerCert;
  FLAGS_key_pem = kTestServerKey;

  tls_socket_server_transport tls_transport{"127.0.0.1", port};
  test_server_app app;

  std::thread server_thread([&] { app.run_with_transport(tls_transport); });
  // Give the server thread a moment to start() (including the TLS handshake
  // machinery being armed).
  std::this_thread::sleep_for(std::chrono::milliseconds{100});

  {
    client c{tls_socket_client_transport{"127.0.0.1", port}};
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
  server_thread.join();

  FLAGS_transport = "tcp";
  FLAGS_cert_pem.clear();
  FLAGS_key_pem.clear();
}

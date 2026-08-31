// MIT License © 2025 Binary Dice Games
// Tests for src/app/client/client_app.hpp

#include "src/app/client/client_app.hpp"
#include "src/rmi/rmi.hpp"
#include "src/rmi/transport/tls_socket_transport.hpp"
#include "tests/tls_test_certs.hpp"

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

// client_app.cpp / transport_flags.cpp DECLARE these flags for CLI-driven
// binaries to DEFINE; this test executable calls run_with_transport()
// directly (bypassing run()'s CLI parsing) but still links the object code
// that references them, so they must be defined here. Mirrors
// bridge_app_tests.cpp's identical rationale.
DEFINE_string(transport, "tcp", "test default");
DEFINE_string(host, "127.0.0.1", "test default");
DEFINE_int32(port, 7070, "test default");
DEFINE_string(name, "", "test default");
DEFINE_int32(timeout, 30000, "test default");
DEFINE_bool(debugger, false, "test default");
DEFINE_string(ca_file, "", "test default");
DEFINE_string(ca_pem, "", "test default");
DEFINE_string(server_name, "", "test default");
DEFINE_bool(insecure_skip_verify, false, "test default");
DEFINE_string(cert_file, "", "test default");
DEFINE_string(cert_pem, "", "test default");
DEFINE_string(key_file, "", "test default");
DEFINE_string(key_pem, "", "test default");
DEFINE_string(key_password, "", "test default");

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

class test_client_app : public app::client_app {
 public:
  using app::client_app::connect_failure_hint;
  using app::client_app::on_connect_params;
  using app::client_app::run_with_transport;

  std::function<int(rmi::client&)> session_fn;
  mutable std::string last_error;

 protected:
  int on_session(rmi::client& c) override {
    return session_fn ? session_fn(c) : 0;
  }

  void on_error(const std::string& msg) const override {
    last_error = msg;
  }
};

// A client transport that opens cleanly but never delivers a frame and reports
// itself disconnected, so the RMI connection handshake can never complete.
class unresponsive_client_transport : public transport::client_transport_iface {
 public:
  void open(dynamic /*params*/) override {}
  void send(buffer /*frame*/) override {}
  bool receive(buffer& /*frame*/, std::chrono::milliseconds /*timeout*/) override {
    return false;
  }
  void shutdown() override {}
  bool is_connected() const override {
    return false;
  }
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// on_connect_params()
// ─────────────────────────────────────────────────────────────────────────────

TEST(ClientAppTest, OnConnectParamsOnlySetsTimeoutForNonTlsTransport) {
  FLAGS_transport = "tcp";
  test_client_app app;
  dynamic params;
  app.on_connect_params(params);
  EXPECT_NE(params.findField("timeout_ms"_key), nullptr);
  EXPECT_EQ(params.findField("ca_pem"_key), nullptr);
}

TEST(ClientAppTest, OnConnectParamsPopulatesTlsFieldsWhenSelected) {
  FLAGS_transport = "tls";
  FLAGS_ca_pem = kTestCaCert;

  test_client_app app;
  dynamic params;
  app.on_connect_params(params);

  EXPECT_EQ(params["ca_pem"_key].as<std::string>(), kTestCaCert);
  EXPECT_EQ(params["insecure_skip_verify"_key].as<bool>(), false);

  FLAGS_transport = "tcp";
  FLAGS_ca_pem.clear();
}

TEST(ClientAppTest, OnConnectParamsOmitsServerNameWhenFlagEmpty) {
  // A present-but-empty "server_name" field would override
  // tls_socket_client_transport::open()'s own fallback to --host with a
  // literal empty hostname (see client_app.cpp's comment on this).
  FLAGS_transport = "tls";
  FLAGS_ca_pem = kTestCaCert;
  FLAGS_server_name = "";

  test_client_app app;
  dynamic params;
  app.on_connect_params(params);

  EXPECT_EQ(params.findField("server_name"_key), nullptr);

  FLAGS_transport = "tcp";
  FLAGS_ca_pem.clear();
}

// ─────────────────────────────────────────────────────────────────────────────
// run_with_transport() end-to-end
// ─────────────────────────────────────────────────────────────────────────────
//
// server::listen(params) is the sole call that starts the transport (see
// tls_socket_server_transport::start()'s doc comment on init ordering) --
// constructing the transport unstarted and letting listen() start it, rather
// than pre-calling transport.start() and then also calling listen(), avoids
// double-initializing the listener.

namespace {

std::atomic<uint16_t> g_client_app_tls_port{29970};

} // namespace

TEST(ClientAppTest, HandshakeAndRmiCallSucceedOverTls) {
  clearClassRegistry();
  registerCalculator();

  tls_socket_server_transport server_transport{"127.0.0.1", g_client_app_tls_port.fetch_add(1)};
  auto server_client_transport = server_transport.connect();

  server server_srv{server_transport};
  server_srv.listen(tls_server_params(kTestServerCert, kTestServerKey));

  FLAGS_transport = "tls";
  FLAGS_ca_pem = kTestCaCert;

  test_client_app app;
  app.session_fn = [](client& c) -> int {
    auto calc = c.instantiate(0U, "Calculator"_key).get();
    EXPECT_TRUE(calc.valid());

    dynamic params;
    params["a"_key] = 10.0f;
    params["b"_key] = 3.0f;
    auto result = calc.call("add"_key, std::move(params)).get();
    float res = result["result"_key];
    EXPECT_FLOAT_EQ(res, 13.0f);

    c.destroy(std::move(calc));
    return 0;
  };

  EXPECT_EQ(
      app.run_with_transport(std::make_unique<tls_socket_client_transport>(std::move(server_client_transport))), 0);

  FLAGS_transport = "tcp";
  FLAGS_ca_pem.clear();
  server_srv.stop();
}

TEST(ClientAppTest, MutualTlsClientCertIsForwardedFromFlags) {
  clearClassRegistry();
  registerCalculator();

  tls_socket_server_transport server_transport{"127.0.0.1", g_client_app_tls_port.fetch_add(1)};
  auto client_transport_obj = server_transport.connect();

  server server_srv{server_transport};
  server_srv.listen(tls_server_params(kTestServerCert, kTestServerKey, "required", kTestCaCert));

  FLAGS_transport = "tls";
  FLAGS_ca_pem = kTestCaCert;
  FLAGS_cert_pem = kTestClientCert;
  FLAGS_key_pem = kTestClientKey;

  test_client_app app;
  app.session_fn = [](client& c) -> int {
    auto calc = c.instantiate(0U, "Calculator"_key).get();
    EXPECT_TRUE(calc.valid());
    c.destroy(std::move(calc));
    return 0;
  };

  EXPECT_EQ(
      app.run_with_transport(std::make_unique<tls_socket_client_transport>(std::move(client_transport_obj))), 0);

  FLAGS_transport = "tcp";
  FLAGS_ca_pem.clear();
  FLAGS_cert_pem.clear();
  FLAGS_key_pem.clear();
  server_srv.stop();
}

// ─────────────────────────────────────────────────────────────────────────────
// connect() failure reporting
// ─────────────────────────────────────────────────────────────────────────────

TEST(ClientAppTest, ConnectFailureReportsCleanHandshakeMessage) {
  FLAGS_transport = "tcp";

  test_client_app app;
  EXPECT_EQ(app.run_with_transport(std::make_unique<unresponsive_client_transport>()), 1);

  // The internal "Worker thread exiting (code=<hash>)" text never surfaces.
  EXPECT_NE(app.last_error.find("no response from the peer"), std::string::npos);
  EXPECT_EQ(app.last_error.find("code="), std::string::npos);
  EXPECT_EQ(app.last_error.find("Worker thread exiting"), std::string::npos);
  // Non-term transports get no extra guidance line.
  EXPECT_EQ(app.last_error.find("No RMI peer detected on this terminal"), std::string::npos);
}

TEST(ClientAppTest, ConnectFailureOverTermTransportAppendsGuidance) {
  FLAGS_transport = "term";

  test_client_app app;
  EXPECT_EQ(app.run_with_transport(std::make_unique<unresponsive_client_transport>()), 1);

  EXPECT_NE(app.last_error.find("no response from the peer"), std::string::npos);
  EXPECT_NE(app.last_error.find("No RMI peer detected on this terminal"), std::string::npos);
  EXPECT_NE(app.last_error.find("--transport=tcp"), std::string::npos);

  FLAGS_transport = "tcp";
}

TEST(ClientAppTest, ConnectFailureHintLeavesNonTermMessagesUnchanged) {
  FLAGS_transport = "tcp";
  test_client_app app;
  EXPECT_EQ(app.connect_failure_hint("boom"), "boom");

  FLAGS_transport = "term";
  EXPECT_NE(app.connect_failure_hint("boom").find("No RMI peer detected on this terminal"), std::string::npos);

  FLAGS_transport = "tcp";
}

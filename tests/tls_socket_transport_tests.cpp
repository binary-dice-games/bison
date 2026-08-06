// MIT License © 2025 Binary Dice Games
// tls_socket_transport unit tests: handshake, send/receive, certificate
// verification (including rejection), and mutual TLS.

#include "src/rmi/transport/tls_socket_transport.hpp"
#include "tests/tls_test_certs.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>

using namespace bdg::bison;
using namespace bdg::bison::rmi::transport;
using namespace bdg::bison::rmi::transport::test;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

dynamic tls_server_params(const std::string& cert_pem, const std::string& key_pem,
                           const std::string& client_auth = "", const std::string& ca_pem = "") {
  dynamic params;
  params["cert_pem"_key] = cert_pem;
  params["key_pem"_key] = key_pem;
  if (!client_auth.empty())
    params["client_auth"_key] = client_auth;
  if (!ca_pem.empty())
    params["ca_pem"_key] = ca_pem;
  return params;
}

dynamic tls_client_params(const std::string& ca_pem, bool insecure_skip_verify = false,
                           const std::string& cert_pem = "", const std::string& key_pem = "") {
  dynamic params;
  if (!ca_pem.empty())
    params["ca_pem"_key] = ca_pem;
  if (insecure_skip_verify)
    params["insecure_skip_verify"_key] = true;
  if (!cert_pem.empty()) {
    params["cert_pem"_key] = cert_pem;
    params["key_pem"_key] = key_pem;
  }
  return params;
}

tls_socket_server_transport make_tls_server_transport(const dynamic& server_params) {
  static std::atomic<uint16_t> next_port{29000};

  for (int attempt = 0; attempt < 64; ++attempt) {
    const auto port = next_port.fetch_add(1);
    tls_socket_server_transport transport{"127.0.0.1", port};
    try {
      transport.start(server_params);
      return transport;
    } catch (const std::runtime_error&) {
    }
  }

  throw std::runtime_error("unable to allocate tls test port");
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// Handshake + framed send/receive
// ═════════════════════════════════════════════════════════════════════════════

TEST(TlsSocketTransport, HandshakeAndSendReceivePair) {
  auto server_transport = make_tls_server_transport(tls_server_params(kTestServerCert, kTestServerKey));
  // connect() only carries over host/port -- the caller still supplies its
  // own trust material to open(), which is exercised here.
  auto client_t = server_transport.connect();
  client_t.open(tls_client_params(kTestCaCert));

  auto server_conn = server_transport.accept(std::chrono::milliseconds{2000});
  ASSERT_TRUE(server_conn != nullptr);

  const buffer frame{'H', 'i'};
  client_t.send(frame);

  buffer received;
  const bool ok = server_conn->receive(received, std::chrono::milliseconds{2000});
  ASSERT_TRUE(ok);
  EXPECT_EQ(received, frame);

  client_t.shutdown();
  server_conn->close();
  server_transport.stop();
}

TEST(TlsSocketTransport, ServerToClientSend) {
  auto server_transport = make_tls_server_transport(tls_server_params(kTestServerCert, kTestServerKey));
  auto client_t = server_transport.connect();
  client_t.open(tls_client_params(kTestCaCert));

  auto server_conn = server_transport.accept(std::chrono::milliseconds{2000});
  ASSERT_TRUE(server_conn != nullptr);

  const buffer reply{'O', 'K'};
  server_conn->send(reply);

  buffer got;
  const bool ok = client_t.receive(got, std::chrono::milliseconds{2000});
  ASSERT_TRUE(ok);
  EXPECT_EQ(got, reply);

  client_t.shutdown();
  server_conn->close();
  server_transport.stop();
}

TEST(TlsSocketTransport, AcceptTimeoutReturnsNullopt) {
  auto server_transport = make_tls_server_transport(tls_server_params(kTestServerCert, kTestServerKey));
  auto result = server_transport.accept(std::chrono::milliseconds{50});
  EXPECT_EQ(result, nullptr);
  server_transport.stop();
}

// ═════════════════════════════════════════════════════════════════════════════
// Server certificate verification
// ═════════════════════════════════════════════════════════════════════════════

TEST(TlsSocketTransport, ClientRejectsUntrustedServerCert) {
  // Server presents a self-signed cert that was never signed by kTestCaCert.
  auto server_transport = make_tls_server_transport(tls_server_params(kUntrustedServerCert, kUntrustedServerKey));
  auto client_t = server_transport.connect();
  EXPECT_THROW(client_t.open(tls_client_params(kTestCaCert)), std::exception);
  server_transport.stop();
}

TEST(TlsSocketTransport, InsecureSkipVerifyAllowsSelfSignedServer) {
  auto server_transport = make_tls_server_transport(tls_server_params(kUntrustedServerCert, kUntrustedServerKey));
  auto client_t = server_transport.connect();
  EXPECT_NO_THROW(client_t.open(tls_client_params(/*ca_pem=*/"", /*insecure_skip_verify=*/true)));

  auto server_conn = server_transport.accept(std::chrono::milliseconds{2000});
  ASSERT_TRUE(server_conn != nullptr);

  client_t.shutdown();
  server_conn->close();
  server_transport.stop();
}

// ═════════════════════════════════════════════════════════════════════════════
// Mutual TLS (client certificates)
// ═════════════════════════════════════════════════════════════════════════════

TEST(TlsSocketTransport, MutualTlsRequiredRejectsClientWithoutCert) {
  auto server_transport =
      make_tls_server_transport(tls_server_params(kTestServerCert, kTestServerKey, "required", kTestCaCert));
  auto client_t = server_transport.connect();

  // No client certificate supplied even though the server requires one.
  // Under TLS 1.3, the client's own handshake can complete locally before it
  // observes the server's rejection alert (the server only aborts after
  // seeing the client's empty Certificate message, and that alert can
  // arrive after the client already considers its side done) -- so the
  // failure may surface either as open() throwing, or as the connection
  // silently never delivering data. Accept either as proof of rejection.
  bool open_threw = false;
  try {
    client_t.open(tls_client_params(kTestCaCert));
  } catch (const std::exception&) {
    open_threw = true;
  }

  if (!open_threw) {
    auto server_conn = server_transport.accept(std::chrono::milliseconds{2000});
    if (server_conn != nullptr) {
      client_t.send(buffer{'x'});
      buffer received;
      EXPECT_FALSE(server_conn->receive(received, std::chrono::milliseconds{500}));
      server_conn->close();
    }
  }

  client_t.shutdown();
  server_transport.stop();
}

TEST(TlsSocketTransport, MutualTlsAcceptsValidClientCert) {
  auto server_transport =
      make_tls_server_transport(tls_server_params(kTestServerCert, kTestServerKey, "required", kTestCaCert));
  auto client_t = server_transport.connect();
  EXPECT_NO_THROW(client_t.open(tls_client_params(kTestCaCert, /*insecure_skip_verify=*/false, kTestClientCert,
                                                   kTestClientKey)));

  auto server_conn = server_transport.accept(std::chrono::milliseconds{2000});
  ASSERT_TRUE(server_conn != nullptr);

  const buffer frame{'M', 'T', 'L', 'S'};
  client_t.send(frame);

  buffer received;
  ASSERT_TRUE(server_conn->receive(received, std::chrono::milliseconds{2000}));
  EXPECT_EQ(received, frame);

  client_t.shutdown();
  server_conn->close();
  server_transport.stop();
}

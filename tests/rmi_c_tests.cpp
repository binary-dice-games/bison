// MIT License © 2025 Binary Dice Games
// Google Test suite for the pure-C RMI shared-library API.

#include "bison_c.h"
#include "rmi_c.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>

// We clear the class registry using the C++ API to avoid state leakage.
#include "src/bison/bison.hpp"
#include "tests/tls_test_certs.hpp"

static void rmi_c_clear_registry() {
  bdg::bison::dynamic::getRegistry().wlock()->clear();
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

/** RAII wrapper so handles are always released even when a test fails early. */
struct ScopedBisonHandle {
  bison_handle h;
  explicit ScopedBisonHandle(bison_handle h) : h(h) {}
  ~ScopedBisonHandle() {
    bison_release(h);
  }
  operator bison_handle() const {
    return h;
  }
};

struct ScopedClientHandle {
  rmi_client_handle h;
  explicit ScopedClientHandle(rmi_client_handle h) : h(h) {}
  ~ScopedClientHandle() {
    rmi_client_release(h);
  }
  operator rmi_client_handle() const {
    return h;
  }
};

struct ScopedServerHandle {
  rmi_server_handle h;
  explicit ScopedServerHandle(rmi_server_handle h) : h(h) {}
  ~ScopedServerHandle() {
    rmi_server_release(h);
  }
  operator rmi_server_handle() const {
    return h;
  }
};

struct ScopedProxyHandle {
  rmi_proxy_handle h;
  explicit ScopedProxyHandle(rmi_proxy_handle h) : h(h) {}
  ~ScopedProxyHandle() {
    rmi_proxy_release(h);
  }
  operator rmi_proxy_handle() const {
    return h;
  }
};

struct ScopedFutureHandle {
  rmi_future_handle h;
  explicit ScopedFutureHandle(rmi_future_handle h) : h(h) {}
  ~ScopedFutureHandle() {
    rmi_future_release(h);
  }
  operator rmi_future_handle() const {
    return h;
  }
};

static bison_hash H(const char* name) {
  return bison_key(name);
}

static rmi_client_handle make_test_client() {
  static std::atomic<uint16_t> next_port{28000};
  return rmi_client_tcp_create("127.0.0.1", next_port.fetch_add(1));
}

static rmi_server_handle make_test_server() {
  static std::atomic<uint16_t> next_port{29000};
  return rmi_server_tcp_create("127.0.0.1", next_port.fetch_add(1));
}

static rmi_client_handle make_test_tls_client() {
  static std::atomic<uint16_t> next_port{28500};
  return rmi_client_tls_create("127.0.0.1", next_port.fetch_add(1));
}

static rmi_server_handle make_test_tls_server() {
  static std::atomic<uint16_t> next_port{29500};
  return rmi_server_tls_create("127.0.0.1", next_port.fetch_add(1));
}

static bison_handle make_tls_server_params(const std::string& cert_pem, const std::string& key_pem) {
  bison_handle params = bison_create(0);
  bison_set_string(params, bison_key("cert_pem"), cert_pem.c_str());
  bison_set_string(params, bison_key("key_pem"), key_pem.c_str());
  return params;
}

static bison_handle make_tls_client_params(const std::string& ca_pem) {
  bison_handle params = bison_create(0);
  bison_set_string(params, bison_key("ca_pem"), ca_pem.c_str());
  return params;
}

/**
 * Create and start listening a TLS server on a fresh port, retrying past
 * bind conflicts, mirroring `make_paired_tcp_server()` above.
 */
static rmi_server_handle make_paired_tls_server(uint16_t* out_port, bison_handle listen_params) {
  static std::atomic<uint16_t> next_port{29900};
  for (int attempt = 0; attempt < 64; ++attempt) {
    uint16_t port = next_port.fetch_add(1);
    rmi_server_handle server = rmi_server_tls_create("127.0.0.1", port);
    if (!server)
      continue;
    if (rmi_server_listen(server, listen_params, nullptr, nullptr) == RMI_OK) {
      *out_port = port;
      return server;
    }
    rmi_server_release(server);
  }
  return nullptr;
}

static void noop_proxy_event_handler(bison_handle, void*) {}

/**
 * Create and start listening a TCP server on a fresh port, retrying past
 * bind conflicts (mirrors `tests/rmi_tests.cpp`'s
 * `make_socket_server_transport()` / `bindings/cpp/tests/rmi_tests.cpp`'s
 * `make_tcp_server()`). The retry loop matters here specifically because
 * CTest's `gtest_discover_tests` runs each `TEST()` as its own process, so
 * a plain per-process port counter can still collide when several test
 * processes start at the same instant and pick the same first port.
 *
 * @p auth_handler/@p auth_user, if given, are forwarded to
 * `rmi_server_listen()`. @p out_port receives the bound port so the caller
 * can create a client on the *same* port for an actual connect round trip.
 */
static rmi_server_handle
make_paired_tcp_server(uint16_t* out_port, rmi_auth_fn auth_handler = nullptr, void* auth_user = nullptr) {
  static std::atomic<uint16_t> next_port{29800};
  for (int attempt = 0; attempt < 64; ++attempt) {
    uint16_t port = next_port.fetch_add(1);
    rmi_server_handle server = rmi_server_tcp_create("127.0.0.1", port);
    if (!server)
      continue;
    if (rmi_server_listen(server, nullptr, auth_handler, auth_user) == RMI_OK) {
      *out_port = port;
      return server;
    }
    rmi_server_release(server);
  }
  return nullptr;
}

// ═════════════════════════════════════════════════════════════════════════════
// 1. Client lifecycle
// ═════════════════════════════════════════════════════════════════════════════

TEST(ClientLifecycleTests, TcpCreateReturnsNonNull) {
  ScopedClientHandle client{make_test_client()};
  EXPECT_NE(client.h, nullptr);
}

TEST(ClientLifecycleTests, TlsCreateReturnsNonNull) {
  ScopedClientHandle client{make_test_tls_client()};
  EXPECT_NE(client.h, nullptr);
}

TEST(ClientLifecycleTests, ReleaseNullIsSafe) {
  rmi_client_release(nullptr); // must not crash
}

TEST(ClientLifecycleTests, DisconnectNullClientReturnsError) {
  rmi_error err = rmi_client_disconnect(nullptr);
  EXPECT_EQ(err, RMI_ERR_NULL);
}

TEST(ClientLifecycleTests, DescribeUnconnectedClientFails) {
  ScopedClientHandle client{make_test_client()};
  bison_handle desc = nullptr;
  rmi_error err = rmi_client_describe(client, 0, 0, &desc);
  EXPECT_NE(err, RMI_OK);
  // desc should remain nullptr
  EXPECT_EQ(desc, nullptr);
}

// ═════════════════════════════════════════════════════════════════════════════
// 2. Server lifecycle
// ═════════════════════════════════════════════════════════════════════════════

TEST(ServerLifecycleTests, TcpCreateReturnsNonNull) {
  ScopedServerHandle server{make_test_server()};
  EXPECT_NE(server.h, nullptr);
}

TEST(ServerLifecycleTests, TlsCreateReturnsNonNull) {
  ScopedServerHandle server{make_test_tls_server()};
  EXPECT_NE(server.h, nullptr);
}

TEST(ServerLifecycleTests, TermCreateReturnsNonNull) {
  // NULL spawns the platform default shell; release() closes the pty master,
  // which delivers EOF to the shell's stdin and lets it exit on its own.
  ScopedServerHandle server{rmi_server_term_create(nullptr)};
  EXPECT_NE(server.h, nullptr);
}

TEST(ServerLifecycleTests, ReleaseNullIsSafe) {
  rmi_server_release(nullptr); // must not crash
}

TEST(ServerLifecycleTests, StopNullServerIsSafe) {
  rmi_server_stop(nullptr); // must not crash
}

TEST(ServerLifecycleTests, ListenSucceedsWithNullParams) {
  ScopedServerHandle server{make_test_server()};
  ASSERT_NE(server.h, nullptr);
  rmi_error err = rmi_server_listen(server, nullptr, nullptr, nullptr);
  EXPECT_EQ(err, RMI_OK);
  rmi_server_stop(server);
}

// ═════════════════════════════════════════════════════════════════════════════
// 3. Parameter passing with bison_handle
// ═════════════════════════════════════════════════════════════════════════════

TEST(ParameterTests, ClientConnectWithParams) {
  ScopedServerHandle server{make_test_server()};
  ASSERT_NE(server.h, nullptr);
  ASSERT_EQ(rmi_server_listen(server, nullptr, nullptr, nullptr), RMI_OK);

  ScopedClientHandle client{make_test_client()};
  ASSERT_NE(client.h, nullptr);

  ScopedBisonHandle params{bison_create(0)};
  ASSERT_NE(params.h, nullptr);
  rmi_error err = rmi_client_connect(client, params);
  // Should succeed or fail gracefully, but not crash
  EXPECT_TRUE(err == RMI_OK || err == RMI_ERR_TRANSPORT);

  rmi_server_stop(server);
}

// ═════════════════════════════════════════════════════════════════════════════
// 3b. TLS transport round trips (rmi_client_tls_create / rmi_server_tls_create)
// ═════════════════════════════════════════════════════════════════════════════

using namespace bdg::bison::rmi::transport::test;

TEST(TlsTransportTests, ServerListenWithCertParamsSucceeds) {
  ScopedServerHandle server{make_test_tls_server()};
  ASSERT_NE(server.h, nullptr);

  ScopedBisonHandle params{make_tls_server_params(kTestServerCert, kTestServerKey)};
  ASSERT_NE(params.h, nullptr);
  EXPECT_EQ(rmi_server_listen(server, params, nullptr, nullptr), RMI_OK);
  rmi_server_stop(server);
}

TEST(TlsTransportTests, ServerListenWithoutCertParamsFails) {
  ScopedServerHandle server{make_test_tls_server()};
  ASSERT_NE(server.h, nullptr);
  EXPECT_NE(rmi_server_listen(server, nullptr, nullptr, nullptr), RMI_OK);
}

TEST(TlsTransportTests, ClientConnectWithTrustedCaSucceeds) {
  ScopedBisonHandle server_params{make_tls_server_params(kTestServerCert, kTestServerKey)};
  uint16_t port = 0;
  ScopedServerHandle server{make_paired_tls_server(&port, server_params)};
  ASSERT_NE(server.h, nullptr);

  ScopedClientHandle client{rmi_client_tls_create("127.0.0.1", port)};
  ASSERT_NE(client.h, nullptr);

  ScopedBisonHandle client_params{make_tls_client_params(kTestCaCert)};
  EXPECT_EQ(rmi_client_connect(client, client_params), RMI_OK);

  rmi_client_disconnect(client);
  rmi_server_stop(server);
}

TEST(TlsTransportTests, ClientConnectWithUntrustedServerCertFails) {
  // Server presents a certificate that isn't signed by kTestCaCert, so the
  // client's verification against that CA must fail the handshake.
  ScopedBisonHandle server_params{make_tls_server_params(kUntrustedServerCert, kUntrustedServerKey)};
  uint16_t port = 0;
  ScopedServerHandle server{make_paired_tls_server(&port, server_params)};
  ASSERT_NE(server.h, nullptr);

  ScopedClientHandle client{rmi_client_tls_create("127.0.0.1", port)};
  ASSERT_NE(client.h, nullptr);

  ScopedBisonHandle client_params{make_tls_client_params(kTestCaCert)};
  EXPECT_EQ(rmi_client_connect(client, client_params), RMI_ERR_TRANSPORT);

  rmi_server_stop(server);
}

// ═════════════════════════════════════════════════════════════════════════════
// 4. Handle null-safety and error handling
// ═════════════════════════════════════════════════════════════════════════════

TEST(ErrorHandlingTests, InstantiateNullClientReturnsError) {
  rmi_proxy_handle proxy = nullptr;
  rmi_error err = rmi_client_instantiate(nullptr, 0, H("TestClass"), nullptr, &proxy);
  EXPECT_EQ(err, RMI_ERR_NULL);
  EXPECT_EQ(proxy, nullptr);
}

TEST(ErrorHandlingTests, CallNullProxyReturnsError) {
  ScopedProxyHandle proxy{nullptr};
  bison_handle result = nullptr;
  rmi_error err = rmi_proxy_call(proxy, H("method"), nullptr, &result, 1000);
  EXPECT_EQ(err, RMI_ERR_NULL);
}

TEST(ErrorHandlingTests, ClearNullProxyReturnsError) {
  ScopedProxyHandle proxy{nullptr};
  rmi_error err = rmi_proxy_clear(proxy, 1000);
  EXPECT_EQ(err, RMI_ERR_NULL);
}

TEST(ErrorHandlingTests, SetNullProxyReturnsError) {
  ScopedProxyHandle proxy{nullptr};
  rmi_error err = rmi_proxy_set(proxy, nullptr, 1000);
  EXPECT_EQ(err, RMI_ERR_NULL);
}

TEST(ErrorHandlingTests, GetNullProxyReturnsError) {
  ScopedProxyHandle proxy{nullptr};
  bison_handle result = nullptr;
  rmi_error err = rmi_proxy_get(proxy, nullptr, &result, 1000);
  EXPECT_EQ(err, RMI_ERR_NULL);
}

TEST(ErrorHandlingTests, GetNullOutputReturnsError) {
  ScopedProxyHandle proxy{nullptr};
  rmi_error err = rmi_proxy_get(proxy, nullptr, nullptr, 1000);
  EXPECT_EQ(err, RMI_ERR_NULL);
}

TEST(ErrorHandlingTests, FutureWaitNullReturnsError) {
  rmi_error err = rmi_future_wait(nullptr, 1000);
  EXPECT_EQ(err, RMI_ERR_NULL);
}

TEST(ErrorHandlingTests, FutureGetDynamicNullReturnsError) {
  rmi_future_handle fut = nullptr;
  bison_handle value = nullptr;
  rmi_error err = rmi_future_get_dynamic(&fut, &value);
  EXPECT_EQ(err, RMI_ERR_NULL);
}

TEST(ErrorHandlingTests, FutureGetProxyNullReturnsError) {
  rmi_future_handle fut = nullptr;
  rmi_proxy_handle value = nullptr;
  rmi_error err = rmi_future_get_proxy(&fut, &value);
  EXPECT_EQ(err, RMI_ERR_NULL);
}

TEST(ErrorHandlingTests, DescribeAsyncNullClientReturnsError) {
  rmi_future_handle fut = nullptr;
  rmi_error err = rmi_client_describe_async(nullptr, 0, 0, &fut);
  EXPECT_EQ(err, RMI_ERR_NULL);
  EXPECT_EQ(fut, nullptr);
}

TEST(ErrorHandlingTests, InstantiateAsyncNullClientReturnsError) {
  rmi_future_handle fut = nullptr;
  rmi_error err = rmi_client_instantiate_async(nullptr, 0, H("TestClass"), nullptr, &fut);
  EXPECT_EQ(err, RMI_ERR_NULL);
  EXPECT_EQ(fut, nullptr);
}

TEST(ErrorHandlingTests, ClearAsyncNullProxyReturnsError) {
  rmi_future_handle fut = nullptr;
  ScopedProxyHandle proxy{nullptr};
  rmi_error err = rmi_proxy_clear_async(proxy, &fut);
  EXPECT_EQ(err, RMI_ERR_NULL);
  EXPECT_EQ(fut, nullptr);
}

TEST(ErrorHandlingTests, SetAsyncNullProxyReturnsError) {
  rmi_future_handle fut = nullptr;
  ScopedProxyHandle proxy{nullptr};
  rmi_error err = rmi_proxy_set_async(proxy, nullptr, &fut);
  EXPECT_EQ(err, RMI_ERR_NULL);
  EXPECT_EQ(fut, nullptr);
}

TEST(ErrorHandlingTests, GetAsyncNullProxyReturnsError) {
  rmi_future_handle fut = nullptr;
  ScopedProxyHandle proxy{nullptr};
  rmi_error err = rmi_proxy_get_async(proxy, nullptr, &fut);
  EXPECT_EQ(err, RMI_ERR_NULL);
  EXPECT_EQ(fut, nullptr);
}

TEST(ErrorHandlingTests, CallAsyncNullProxyReturnsError) {
  rmi_future_handle fut = nullptr;
  ScopedProxyHandle proxy{nullptr};
  rmi_error err = rmi_proxy_call_async(proxy, H("method"), nullptr, &fut);
  EXPECT_EQ(err, RMI_ERR_NULL);
  EXPECT_EQ(fut, nullptr);
}

TEST(ErrorHandlingTests, ProxyReleaseNullIsSafe) {
  rmi_proxy_release(nullptr); // must not crash
}

TEST(ErrorHandlingTests, ProxyOnEventNullProxyReturnsError) {
  rmi_error err = rmi_proxy_on_event(nullptr, H("evt"), noop_proxy_event_handler, nullptr);
  EXPECT_EQ(err, RMI_ERR_NULL);
}

TEST(ErrorHandlingTests, ProxyOnEventNullHandlerReturnsError) {
  rmi_error err = rmi_proxy_on_event(nullptr, H("evt"), nullptr, nullptr);
  EXPECT_EQ(err, RMI_ERR_NULL);
}

// ═════════════════════════════════════════════════════════════════════════════
// 5. Bison parameter integration
// ═════════════════════════════════════════════════════════════════════════════

TEST(BisonIntegrationTests, ConnectWithBisonParams) {
  ScopedClientHandle client{make_test_client()};
  ASSERT_NE(client.h, nullptr);

  ScopedBisonHandle params{bison_create(0)};
  ASSERT_NE(params.h, nullptr);
  ASSERT_EQ(bison_set_int(params, H("timeout"), 5000), BISON_OK);

  // Should handle params gracefully
  rmi_error err = rmi_client_connect(client, params);
  EXPECT_TRUE(err == RMI_OK || err == RMI_ERR_TRANSPORT);
}

TEST(BisonIntegrationTests, ListenWithBisonParams) {
  ScopedServerHandle server{make_test_server()};
  ASSERT_NE(server.h, nullptr);

  ScopedBisonHandle params{bison_create(0)};
  ASSERT_NE(params.h, nullptr);
  ASSERT_EQ(bison_set_int(params, H("backlog"), 5), BISON_OK);

  rmi_error err = rmi_server_listen(server, params, nullptr, nullptr);
  EXPECT_EQ(err, RMI_OK);
  rmi_server_stop(server);
}

// ═════════════════════════════════════════════════════════════════════════════
// 6. Timeout behavior
// ═════════════════════════════════════════════════════════════════════════════

TEST(TimeoutTests, ProxyCallWithTimeoutMs) {
  // This test just verifies the API accepts timeout_ms; actual timeout testing
  // requires a working server/client pair.
  // We verify the signature compiles and accepts -1 (no timeout) and positive
  // values.
  bison_handle dummy_result = nullptr;
  int64_t timeout_vals[] = {-1, 0, 1000, 5000};
  for (int64_t tv : timeout_vals) {
    // With null proxy, should fail with RMI_ERR_NULL, not timeout.
    rmi_error err = rmi_proxy_call(nullptr, H("m"), nullptr, &dummy_result, tv);
    EXPECT_EQ(err, RMI_ERR_NULL);
  }
}

TEST(TimeoutTests, ProxyClearWithTimeoutMs) {
  int64_t timeout_vals[] = {-1, 0, 1000, 5000};
  for (int64_t tv : timeout_vals) {
    rmi_error err = rmi_proxy_clear(nullptr, tv);
    EXPECT_EQ(err, RMI_ERR_NULL);
  }
}

TEST(TimeoutTests, ProxySetWithTimeoutMs) {
  int64_t timeout_vals[] = {-1, 0, 1000, 5000};
  for (int64_t tv : timeout_vals) {
    rmi_error err = rmi_proxy_set(nullptr, nullptr, tv);
    EXPECT_EQ(err, RMI_ERR_NULL);
  }
}

TEST(TimeoutTests, ProxyGetWithTimeoutMs) {
  bison_handle dummy_result = nullptr;
  int64_t timeout_vals[] = {-1, 0, 1000, 5000};
  for (int64_t tv : timeout_vals) {
    rmi_error err = rmi_proxy_get(nullptr, nullptr, &dummy_result, tv);
    EXPECT_EQ(err, RMI_ERR_NULL);
  }
}

// ═════════════════════════════════════════════════════════════════════════════
// 6b. Authentication (rmi_server_listen()'s auth_handler/auth_user)
// ═════════════════════════════════════════════════════════════════════════════

namespace {

bool accepting_auth_handler(bison_handle payload, char* identity_buf, size_t identity_buf_len, void* user) {
  auto* seen_username = static_cast<std::string*>(user);
  char buf[64] = {0};
  size_t len = 0;
  if (seen_username && bison_get_string(payload, bison_key("username"), buf, sizeof(buf), &len) == BISON_OK)
    seen_username->assign(buf, len);
  const char identity[] = "alice-id";
  std::strncpy(identity_buf, identity, identity_buf_len - 1);
  identity_buf[identity_buf_len - 1] = '\0';
  return true;
}

bool rejecting_auth_handler(bison_handle, char*, size_t, void*) {
  return false;
}

} // namespace

TEST(RmiAuthTests, NoAuthHandlerConnectSucceeds) {
  uint16_t port = 0;
  ScopedServerHandle server{make_paired_tcp_server(&port)};
  ASSERT_NE(server.h, nullptr);

  ScopedClientHandle client{rmi_client_tcp_create("127.0.0.1", port)};
  ASSERT_NE(client.h, nullptr);
  EXPECT_EQ(rmi_client_connect(client, nullptr), RMI_OK);
}

TEST(RmiAuthTests, AcceptingHandlerReceivesPayloadAndConnectSucceeds) {
  uint16_t port = 0;
  std::string seen_username;
  ScopedServerHandle server{make_paired_tcp_server(&port, accepting_auth_handler, &seen_username)};
  ASSERT_NE(server.h, nullptr);

  ScopedClientHandle client{rmi_client_tcp_create("127.0.0.1", port)};
  ASSERT_NE(client.h, nullptr);

  ScopedBisonHandle params{bison_create(0)};
  ASSERT_NE(params.h, nullptr);
  ASSERT_EQ(bison_set_string(params, H("username"), "alice"), BISON_OK);

  EXPECT_EQ(rmi_client_connect(client, params), RMI_OK);
  EXPECT_EQ(seen_username, "alice");
}

TEST(RmiAuthTests, RejectingHandlerFailsConnect) {
  uint16_t port = 0;
  ScopedServerHandle server{make_paired_tcp_server(&port, rejecting_auth_handler, nullptr)};
  ASSERT_NE(server.h, nullptr);

  ScopedClientHandle client{rmi_client_tcp_create("127.0.0.1", port)};
  ASSERT_NE(client.h, nullptr);

  EXPECT_NE(rmi_client_connect(client, nullptr), RMI_OK);
}

TEST(RmiAuthTests, ListenNullServerReturnsError) {
  EXPECT_EQ(rmi_server_listen(nullptr, nullptr, accepting_auth_handler, nullptr), RMI_ERR_NULL);
}

// ═════════════════════════════════════════════════════════════════════════════
// 7. Standalone in-process client
// ═════════════════════════════════════════════════════════════════════════════

class StandaloneAbiTests : public ::testing::Test {
 protected:
  void SetUp() override {
    // Clear the class registry before each test to avoid cross-test pollution.
    rmi_c_clear_registry();
  }

  void TearDown() override {
    rmi_c_clear_registry();
  }

  /** Register a simple "Counter" class with a single int32 field "value". */
  bool register_counter_class() {
    uint32_t key = bison_key("StandaloneCounter");
    bison_handle proto = bison_create(key);
    if (!proto)
      return false;
    bison_set_int(proto, bison_key("value"), 0);
    bool ok = bison_add_class(0, proto, 0, nullptr) == BISON_OK;
    bison_release(proto);
    return ok;
  }
};

TEST_F(StandaloneAbiTests, CreateReturnsNonNull) {
  ScopedClientHandle sa{rmi_standalone_create()};
  EXPECT_NE(sa.h, nullptr);
}

TEST_F(StandaloneAbiTests, ConnectIsNoOpReturnsOk) {
  ScopedClientHandle sa{rmi_standalone_create()};
  ASSERT_NE(sa.h, nullptr);
  EXPECT_EQ(rmi_client_connect(sa, nullptr), RMI_OK);
}

TEST_F(StandaloneAbiTests, DisconnectIsNoOpReturnsOk) {
  ScopedClientHandle sa{rmi_standalone_create()};
  ASSERT_NE(sa.h, nullptr);
  EXPECT_EQ(rmi_client_disconnect(sa), RMI_OK);
}

TEST_F(StandaloneAbiTests, DescribeAllReturnsHandle) {
  ASSERT_TRUE(register_counter_class());

  ScopedClientHandle sa{rmi_standalone_create()};
  ASSERT_NE(sa.h, nullptr);

  bison_handle desc = nullptr;
  EXPECT_EQ(rmi_client_describe(sa, 0, 0, &desc), RMI_OK);
  EXPECT_NE(desc, nullptr);
  bison_release(desc);
}

TEST_F(StandaloneAbiTests, InstantiateAndProxyGetField) {
  ASSERT_TRUE(register_counter_class());

  ScopedClientHandle sa{rmi_standalone_create()};
  ASSERT_NE(sa.h, nullptr);

  rmi_proxy_handle proxy = nullptr;
  ASSERT_EQ(rmi_client_instantiate(sa, 0, bison_key("StandaloneCounter"), nullptr, &proxy), RMI_OK);
  ASSERT_NE(proxy, nullptr);

  bison_handle snap = nullptr;
  ASSERT_EQ(rmi_proxy_get(proxy, nullptr, &snap, -1), RMI_OK);
  ASSERT_NE(snap, nullptr);

  int32_t value = 0;
  EXPECT_EQ(bison_get_int(snap, bison_key("value"), &value), BISON_OK);
  EXPECT_EQ(value, 0);

  bison_release(snap);
  rmi_proxy_release(proxy);
}

TEST_F(StandaloneAbiTests, ProxySetAndGet) {
  ASSERT_TRUE(register_counter_class());

  ScopedClientHandle sa{rmi_standalone_create()};
  ASSERT_NE(sa.h, nullptr);

  rmi_proxy_handle proxy = nullptr;
  ASSERT_EQ(rmi_client_instantiate(sa, 0, bison_key("StandaloneCounter"), nullptr, &proxy), RMI_OK);
  ASSERT_NE(proxy, nullptr);

  // Set value to 42.
  ScopedBisonHandle fields{bison_create(0)};
  ASSERT_NE(fields.h, nullptr);
  ASSERT_EQ(bison_set_int(fields, bison_key("value"), 42), BISON_OK);
  EXPECT_EQ(rmi_proxy_set(proxy, fields, -1), RMI_OK);

  // Read it back.
  bison_handle snap = nullptr;
  ASSERT_EQ(rmi_proxy_get(proxy, nullptr, &snap, -1), RMI_OK);
  ASSERT_NE(snap, nullptr);

  int32_t value = 0;
  EXPECT_EQ(bison_get_int(snap, bison_key("value"), &value), BISON_OK);
  EXPECT_EQ(value, 42);

  bison_release(snap);
  rmi_proxy_release(proxy);
}

TEST_F(StandaloneAbiTests, ProxyClearResetsField) {
  ASSERT_TRUE(register_counter_class());

  ScopedClientHandle sa{rmi_standalone_create()};
  ASSERT_NE(sa.h, nullptr);

  rmi_proxy_handle proxy = nullptr;
  ASSERT_EQ(rmi_client_instantiate(sa, 0, bison_key("StandaloneCounter"), nullptr, &proxy), RMI_OK);

  ScopedBisonHandle fields{bison_create(0)};
  bison_set_int(fields, bison_key("value"), 99);
  EXPECT_EQ(rmi_proxy_set(proxy, fields, -1), RMI_OK);

  EXPECT_EQ(rmi_proxy_clear(proxy, -1), RMI_OK);

  bison_handle snap = nullptr;
  ASSERT_EQ(rmi_proxy_get(proxy, nullptr, &snap, -1), RMI_OK);
  int32_t value = -1;
  bison_get_int(snap, bison_key("value"), &value);
  EXPECT_EQ(value, 0);

  bison_release(snap);
  rmi_proxy_release(proxy);
}

TEST_F(StandaloneAbiTests, InstantiateUnregisteredClassFails) {
  ScopedClientHandle sa{rmi_standalone_create()};
  ASSERT_NE(sa.h, nullptr);

  rmi_proxy_handle proxy = nullptr;
  rmi_error err = rmi_client_instantiate(sa, 0, bison_key("NoSuchClass"), nullptr, &proxy);
  EXPECT_NE(err, RMI_OK);
  EXPECT_EQ(proxy, nullptr);
}

// ═════════════════════════════════════════════════════════════════════════════
// 8. Describe attribute metadata via pure C ABI
// ═════════════════════════════════════════════════════════════════════════════

class DescribeAbiTests : public ::testing::Test {
 protected:
  void SetUp() override {
    rmi_c_clear_registry();
  }

  void TearDown() override {
    rmi_c_clear_registry();
  }
};

// Helper: copy a bison_get_string result into a std::string.
static std::string read_string(bison_handle h, bison_hash name) {
  size_t len = 0;
  if (bison_get_string(h, name, nullptr, 0, &len) != BISON_OK)
    return {};
  std::string s(len + 1, '\0');
  bison_get_string(h, name, s.data(), s.size(), nullptr);
  s.resize(len);
  return s;
}

TEST_F(DescribeAbiTests, DescribeSpecificClassIncludesClassAttributes) {
  // Register a class with class-level attributes using the pure C ABI.
  ScopedBisonHandle proto{bison_create(bison_key("CWidgetClass"))};
  ASSERT_NE(proto.h, nullptr);
  bison_set_int(proto, bison_key("x"), 0);
  bison_set_int(proto, bison_key("y"), 0);

  bison_attributes meta{};
  meta.display_name = "C Widget";
  meta.description = "A C-facing widget class";
  meta.category = "C UI";
  ASSERT_EQ(bison_add_class(0, proto, 0, &meta), BISON_OK);

  ScopedClientHandle sa{rmi_standalone_create()};
  ASSERT_NE(sa.h, nullptr);

  bison_handle desc = nullptr;
  ASSERT_EQ(rmi_client_describe(sa, 0, bison_key("CWidgetClass"), &desc), RMI_OK);
  ASSERT_NE(desc, nullptr);

  EXPECT_EQ(read_string(desc, bison_key("__displayName")), "C Widget");
  EXPECT_EQ(read_string(desc, bison_key("__description")), "A C-facing widget class");
  EXPECT_EQ(read_string(desc, bison_key("__category")), "C UI");

  bison_release(desc);
}

TEST_F(DescribeAbiTests, DescribeSpecificClassIncludesObsoleteFlag) {
  ScopedBisonHandle proto{bison_create(bison_key("LegacyClass"))};
  ASSERT_NE(proto.h, nullptr);
  bison_set_int(proto, bison_key("v"), 0);

  bison_attributes meta{};
  meta.obsolete = 1;
  meta.obsolete_message = "Use NewClass instead";
  ASSERT_EQ(bison_add_class(0, proto, 0, &meta), BISON_OK);

  ScopedClientHandle sa{rmi_standalone_create()};
  ASSERT_NE(sa.h, nullptr);

  bison_handle desc = nullptr;
  ASSERT_EQ(rmi_client_describe(sa, 0, bison_key("LegacyClass"), &desc), RMI_OK);
  ASSERT_NE(desc, nullptr);

  int obsolete_flag = 0;
  EXPECT_EQ(bison_get_bool(desc, bison_key("__obsolete"), &obsolete_flag), BISON_OK);
  EXPECT_NE(obsolete_flag, 0);
  EXPECT_EQ(read_string(desc, bison_key("__obsoleteMessage")), "Use NewClass instead");

  bison_release(desc);
}

TEST_F(DescribeAbiTests, DescribeSpecificClassFieldMetadataInFieldsObject) {
  ScopedBisonHandle proto{bison_create(bison_key("CPlayerClass"))};
  ASSERT_NE(proto.h, nullptr);

  bison_attributes name_meta{};
  name_meta.display_name = "Player Name";
  name_meta.description = "Full name";
  name_meta.required = 1;
  ASSERT_EQ(bison_add_field_string(proto, bison_key("name"), "", &name_meta), BISON_OK);

  bison_attributes score_meta{};
  score_meta.display_name = "Score";
  score_meta.obsolete = 1;
  score_meta.obsolete_message = "Use rating instead";
  ASSERT_EQ(bison_add_field_int(proto, bison_key("score"), 0, &score_meta), BISON_OK);

  bison_attributes class_meta{};
  class_meta.display_name = "C Player";
  class_meta.category = "Gameplay";
  ASSERT_EQ(bison_add_class(0, proto, 0, &class_meta), BISON_OK);

  ScopedClientHandle sa{rmi_standalone_create()};
  ASSERT_NE(sa.h, nullptr);

  bison_handle desc = nullptr;
  ASSERT_EQ(rmi_client_describe(sa, 0, bison_key("CPlayerClass"), &desc), RMI_OK);
  ASSERT_NE(desc, nullptr);

  // Class-level attrs.
  EXPECT_EQ(read_string(desc, bison_key("__displayName")), "C Player");
  EXPECT_EQ(read_string(desc, bison_key("__category")), "Gameplay");

  // Get the __fields map.
  bison_handle fields_map = nullptr;
  ASSERT_EQ(bison_get_object(desc, bison_key("__fields"), &fields_map), BISON_OK);
  ASSERT_NE(fields_map, nullptr);

  // name field metadata.
  bison_handle name_desc = nullptr;
  ASSERT_EQ(bison_get_object(fields_map, bison_key("name"), &name_desc), BISON_OK);
  ASSERT_NE(name_desc, nullptr);
  EXPECT_EQ(read_string(name_desc, bison_key("__displayName")), "Player Name");
  EXPECT_EQ(read_string(name_desc, bison_key("__description")), "Full name");
  int required_flag = 0;
  EXPECT_EQ(bison_get_bool(name_desc, bison_key("__required"), &required_flag), BISON_OK);
  EXPECT_NE(required_flag, 0);
  bison_release(name_desc);

  // score field metadata.
  bison_handle score_desc = nullptr;
  ASSERT_EQ(bison_get_object(fields_map, bison_key("score"), &score_desc), BISON_OK);
  ASSERT_NE(score_desc, nullptr);
  EXPECT_EQ(read_string(score_desc, bison_key("__displayName")), "Score");
  int obs_flag = 0;
  EXPECT_EQ(bison_get_bool(score_desc, bison_key("__obsolete"), &obs_flag), BISON_OK);
  EXPECT_NE(obs_flag, 0);
  EXPECT_EQ(read_string(score_desc, bison_key("__obsoleteMessage")), "Use rating instead");
  bison_release(score_desc);

  bison_release(fields_map);
  bison_release(desc);
}

TEST_F(DescribeAbiTests, DescribeAllReturnsNonEmptyResult) {
  ScopedBisonHandle proto{bison_create(bison_key("AnnotatedCClass"))};
  ASSERT_NE(proto.h, nullptr);
  bison_set_int(proto, bison_key("v"), 0);

  bison_attributes meta{};
  meta.display_name = "Annotated C Class";
  meta.category = "Testing";
  ASSERT_EQ(bison_add_class(0, proto, 0, &meta), BISON_OK);

  ScopedClientHandle sa{rmi_standalone_create()};
  ASSERT_NE(sa.h, nullptr);

  bison_handle all_desc = nullptr;
  ASSERT_EQ(rmi_client_describe(sa, 0, 0, &all_desc), RMI_OK);
  ASSERT_NE(all_desc, nullptr);

  // The result is an indexed array; at least one entry must be present.
  EXPECT_GT(bison_size(all_desc), 0u);

  bison_release(all_desc);
}

TEST_F(DescribeAbiTests, AddFieldWithRequiredAttributeAppearsInDescribe) {
  ScopedBisonHandle proto{bison_create(bison_key("CharacterStats"))};
  ASSERT_NE(proto.h, nullptr);

  bison_attributes fmeta{};
  fmeta.display_name = "Health";
  fmeta.required = 1;
  ASSERT_EQ(bison_add_field_int(proto, bison_key("health"), 0, &fmeta), BISON_OK);

  ASSERT_EQ(bison_add_class(0, proto, 0, nullptr), BISON_OK);

  ScopedClientHandle sa{rmi_standalone_create()};
  ASSERT_NE(sa.h, nullptr);

  bison_handle desc = nullptr;
  ASSERT_EQ(rmi_client_describe(sa, 0, bison_key("CharacterStats"), &desc), RMI_OK);
  ASSERT_NE(desc, nullptr);

  bison_handle fields_map = nullptr;
  ASSERT_EQ(bison_get_object(desc, bison_key("__fields"), &fields_map), BISON_OK);
  ASSERT_NE(fields_map, nullptr);

  bison_handle health_meta = nullptr;
  ASSERT_EQ(bison_get_object(fields_map, bison_key("health"), &health_meta), BISON_OK);
  ASSERT_NE(health_meta, nullptr);
  EXPECT_EQ(read_string(health_meta, bison_key("__displayName")), "Health");
  int req = 0;
  EXPECT_EQ(bison_get_bool(health_meta, bison_key("__required"), &req), BISON_OK);
  EXPECT_NE(req, 0);

  bison_release(health_meta);
  bison_release(fields_map);
  bison_release(desc);
}

static void noop_method(bison_handle /*self*/, bison_handle /*params*/, bison_handle /*result*/, void* /*user*/) {}

TEST_F(DescribeAbiTests, DescribeSpecificClassIncludesMethodMetadata) {
  ScopedBisonHandle proto{bison_create(bison_key("ServiceApi"))};
  ASSERT_NE(proto.h, nullptr);

  bison_attributes start_meta{};
  start_meta.display_name = "Start Service";
  start_meta.description = "Starts the background service";
  ASSERT_EQ(bison_add_method(proto, bison_key("start"), noop_method, nullptr, &start_meta), BISON_OK);

  bison_attributes stop_meta{};
  stop_meta.obsolete = 1;
  stop_meta.obsolete_message = "Use shutdown instead";
  ASSERT_EQ(bison_add_method(proto, bison_key("stop"), noop_method, nullptr, &stop_meta), BISON_OK);

  // "ping" — registered without attributes, must still appear in __methods.
  ASSERT_EQ(bison_add_method(proto, bison_key("ping"), noop_method, nullptr, nullptr), BISON_OK);

  ASSERT_EQ(bison_add_class(0, proto, 0, nullptr), BISON_OK);

  ScopedClientHandle sa{rmi_standalone_create()};
  ASSERT_NE(sa.h, nullptr);

  bison_handle desc = nullptr;
  ASSERT_EQ(rmi_client_describe(sa, 0, bison_key("ServiceApi"), &desc), RMI_OK);
  ASSERT_NE(desc, nullptr);

  bison_handle methods_map = nullptr;
  ASSERT_EQ(bison_get_object(desc, bison_key("__methods"), &methods_map), BISON_OK);
  ASSERT_NE(methods_map, nullptr);

  // "start" — display name and description.
  bison_handle start_desc = nullptr;
  ASSERT_EQ(bison_get_object(methods_map, bison_key("start"), &start_desc), BISON_OK);
  ASSERT_NE(start_desc, nullptr);
  EXPECT_EQ(read_string(start_desc, bison_key("__displayName")), "Start Service");
  EXPECT_EQ(read_string(start_desc, bison_key("__description")), "Starts the background service");
  bison_release(start_desc);

  // "stop" — obsolete.
  bison_handle stop_desc = nullptr;
  ASSERT_EQ(bison_get_object(methods_map, bison_key("stop"), &stop_desc), BISON_OK);
  ASSERT_NE(stop_desc, nullptr);
  int obs = 0;
  EXPECT_EQ(bison_get_bool(stop_desc, bison_key("__obsolete"), &obs), BISON_OK);
  EXPECT_NE(obs, 0);
  EXPECT_EQ(read_string(stop_desc, bison_key("__obsoleteMessage")), "Use shutdown instead");
  bison_release(stop_desc);

  // "ping" — no attributes, still listed under __methods.
  bison_handle ping_desc = nullptr;
  ASSERT_EQ(bison_get_object(methods_map, bison_key("ping"), &ping_desc), BISON_OK);
  EXPECT_NE(ping_desc, nullptr);
  bison_release(ping_desc);

  bison_release(methods_map);
  bison_release(desc);
}

// ═════════════════════════════════════════════════════════════════════════════
// 9. Profiling
// ═════════════════════════════════════════════════════════════════════════════

TEST(ProfilingAbiTests, EnableProfilingNullServerReturnsError) {
  EXPECT_EQ(rmi_server_enable_profiling(nullptr, "some/dir"), RMI_ERR_NULL);
}

TEST(ProfilingAbiTests, EnableProfilingNullOutputDirReturnsError) {
  ScopedServerHandle server{make_test_server()};
  ASSERT_NE(server.h, nullptr);
  EXPECT_EQ(rmi_server_enable_profiling(server, nullptr), RMI_ERR_NULL);
}

TEST(ProfilingAbiTests, StartCaptureNullServerReturnsError) {
  bool started = false;
  EXPECT_EQ(rmi_server_start_capture(nullptr, "label", &started), RMI_ERR_NULL);
}

TEST(ProfilingAbiTests, StopCaptureNullServerReturnsError) {
  EXPECT_EQ(rmi_server_stop_capture(nullptr), RMI_ERR_NULL);
}

TEST(ProfilingAbiTests, IsCaptureActiveNullServerReturnsError) {
  bool active = false;
  EXPECT_EQ(rmi_server_is_capture_active(nullptr, &active), RMI_ERR_NULL);
}

TEST(ProfilingAbiTests, IsCaptureActiveNullOutputReturnsError) {
  ScopedServerHandle server{make_test_server()};
  ASSERT_NE(server.h, nullptr);
  EXPECT_EQ(rmi_server_is_capture_active(server, nullptr), RMI_ERR_NULL);
}

TEST(ProfilingAbiTests, EnableStartStopCaptureRoundTrips) {
  ScopedServerHandle server{make_test_server()};
  ASSERT_NE(server.h, nullptr);

  ASSERT_EQ(rmi_server_enable_profiling(server, "."), RMI_OK);

  bool started = false;
  ASSERT_EQ(rmi_server_start_capture(server, "abi_test", &started), RMI_OK);
  EXPECT_TRUE(started);

  bool active = false;
  ASSERT_EQ(rmi_server_is_capture_active(server, &active), RMI_OK);
  EXPECT_TRUE(active);

  EXPECT_EQ(rmi_server_stop_capture(server), RMI_OK);

  active = true;
  ASSERT_EQ(rmi_server_is_capture_active(server, &active), RMI_OK);
  EXPECT_FALSE(active);
}

TEST(ProfilingAbiTests, TraceIsActiveFalseWithoutCapture) {
  // No server in this process has enabled profiling/started capture, so the
  // process-wide recorder must be absent.
  EXPECT_FALSE(rmi_trace_is_active());
}

TEST(ProfilingAbiTests, TraceFunctionsAreSafeNoOpsWithoutActiveCapture) {
  // With no active recorder, these must not crash and must simply do nothing.
  rmi_trace_scope_begin("scope");
  rmi_trace_scope_end();
  rmi_trace_instant("instant");
  rmi_trace_counter_int("counter_i", 7);
  rmi_trace_counter_double("counter_d", 1.5);
}

TEST_F(DescribeAbiTests, GetMethodAttributesReturnsCorrectMeta) {
  ScopedBisonHandle proto{bison_create(bison_key("AttrCheckClass"))};
  ASSERT_NE(proto.h, nullptr);

  bison_attributes in{};
  in.display_name = "Compute";
  in.category = "Math";
  in.required = 1;
  ASSERT_EQ(bison_add_method(proto, bison_key("compute"), noop_method, nullptr, &in), BISON_OK);

  bison_attributes out{};
  ASSERT_EQ(bison_get_method_attributes(proto, bison_key("compute"), &out), BISON_OK);
  EXPECT_STREQ(out.display_name, "Compute");
  EXPECT_STREQ(out.category, "Math");
  EXPECT_EQ(out.required, 1);

  EXPECT_EQ(bison_get_method_attributes(proto, bison_key("missing"), &out), BISON_ERR_NOT_FOUND);
}

// MIT License © 2025 Binary Dice Games
// Google Test suite for the pure-C RMI shared-library API.

#include "bison_c.h"
#include "rmi_c.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>

// We clear the class registry using the C++ API to avoid state leakage.
#include "src/core/bison.hpp"
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

static void noop_proxy_event_handler(bison_handle, void*) {}

static int noop_pty_client_on_session(rmi_client_handle, void*) {
  return 0;
}

static void noop_pty_client_on_error(const char*, void*) {}

static void noop_pty_server_register_classes(void*) {}

static void noop_pty_server_on_error(const char*, void*) {}

// ═════════════════════════════════════════════════════════════════════════════
// 1. Client lifecycle
// ═════════════════════════════════════════════════════════════════════════════

TEST(ClientLifecycleTests, TcpCreateReturnsNonNull) {
  ScopedClientHandle client{make_test_client()};
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
  rmi_error err = rmi_client_describe(client, 0, &desc);
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

TEST(ServerLifecycleTests, ReleaseNullIsSafe) {
  rmi_server_release(nullptr); // must not crash
}

TEST(ServerLifecycleTests, StopNullServerIsSafe) {
  rmi_server_stop(nullptr); // must not crash
}

TEST(ServerLifecycleTests, ListenSucceedsWithNullParams) {
  ScopedServerHandle server{make_test_server()};
  ASSERT_NE(server.h, nullptr);
  rmi_error err = rmi_server_listen(server, nullptr);
  EXPECT_EQ(err, RMI_OK);
  rmi_server_stop(server);
}

// ═════════════════════════════════════════════════════════════════════════════
// 3. Parameter passing with bison_handle
// ═════════════════════════════════════════════════════════════════════════════

TEST(ParameterTests, ClientConnectWithParams) {
  ScopedServerHandle server{make_test_server()};
  ASSERT_NE(server.h, nullptr);
  ASSERT_EQ(rmi_server_listen(server, nullptr), RMI_OK);

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
// 4. Handle null-safety and error handling
// ═════════════════════════════════════════════════════════════════════════════

TEST(ErrorHandlingTests, InstantiateNullClientReturnsError) {
  rmi_proxy_handle proxy = nullptr;
  rmi_error err =
      rmi_client_instantiate(nullptr, H("TestClass"), nullptr, &proxy);
  EXPECT_EQ(err, RMI_ERR_NULL);
  EXPECT_EQ(proxy, nullptr);
}

TEST(ErrorHandlingTests, CallNullClientReturnsError) {
  ScopedProxyHandle proxy{nullptr};
  bison_handle result = nullptr;
  rmi_error err =
      rmi_proxy_call(nullptr, proxy, H("method"), nullptr, &result, 1000);
  EXPECT_EQ(err, RMI_ERR_NULL);
}

TEST(ErrorHandlingTests, ClearNullClientReturnsError) {
  ScopedProxyHandle proxy{nullptr};
  rmi_error err = rmi_proxy_clear(nullptr, proxy, 1000);
  EXPECT_EQ(err, RMI_ERR_NULL);
}

TEST(ErrorHandlingTests, SetNullClientReturnsError) {
  ScopedProxyHandle proxy{nullptr};
  rmi_error err = rmi_proxy_set(nullptr, proxy, nullptr, 1000);
  EXPECT_EQ(err, RMI_ERR_NULL);
}

TEST(ErrorHandlingTests, GetNullClientReturnsError) {
  ScopedProxyHandle proxy{nullptr};
  bison_handle result = nullptr;
  rmi_error err = rmi_proxy_get(nullptr, proxy, nullptr, &result, 1000);
  EXPECT_EQ(err, RMI_ERR_NULL);
}

TEST(ErrorHandlingTests, GetNullOutputReturnsError) {
  ScopedProxyHandle proxy{nullptr};
  rmi_error err = rmi_proxy_get(nullptr, proxy, nullptr, nullptr, 1000);
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
  rmi_error err = rmi_client_describe_async(nullptr, 0, &fut);
  EXPECT_EQ(err, RMI_ERR_NULL);
  EXPECT_EQ(fut, nullptr);
}

TEST(ErrorHandlingTests, InstantiateAsyncNullClientReturnsError) {
  rmi_future_handle fut = nullptr;
  rmi_error err =
      rmi_client_instantiate_async(nullptr, H("TestClass"), nullptr, &fut);
  EXPECT_EQ(err, RMI_ERR_NULL);
  EXPECT_EQ(fut, nullptr);
}

TEST(ErrorHandlingTests, ClearAsyncNullClientReturnsError) {
  rmi_future_handle fut = nullptr;
  ScopedProxyHandle proxy{nullptr};
  rmi_error err = rmi_proxy_clear_async(nullptr, proxy, &fut);
  EXPECT_EQ(err, RMI_ERR_NULL);
  EXPECT_EQ(fut, nullptr);
}

TEST(ErrorHandlingTests, SetAsyncNullClientReturnsError) {
  rmi_future_handle fut = nullptr;
  ScopedProxyHandle proxy{nullptr};
  rmi_error err = rmi_proxy_set_async(nullptr, proxy, nullptr, &fut);
  EXPECT_EQ(err, RMI_ERR_NULL);
  EXPECT_EQ(fut, nullptr);
}

TEST(ErrorHandlingTests, GetAsyncNullClientReturnsError) {
  rmi_future_handle fut = nullptr;
  ScopedProxyHandle proxy{nullptr};
  rmi_error err = rmi_proxy_get_async(nullptr, proxy, nullptr, &fut);
  EXPECT_EQ(err, RMI_ERR_NULL);
  EXPECT_EQ(fut, nullptr);
}

TEST(ErrorHandlingTests, CallAsyncNullClientReturnsError) {
  rmi_future_handle fut = nullptr;
  ScopedProxyHandle proxy{nullptr};
  rmi_error err =
      rmi_proxy_call_async(nullptr, proxy, H("method"), nullptr, &fut);
  EXPECT_EQ(err, RMI_ERR_NULL);
  EXPECT_EQ(fut, nullptr);
}

TEST(ErrorHandlingTests, ProxyReleaseNullIsSafe) {
  rmi_proxy_release(nullptr); // must not crash
}

TEST(ErrorHandlingTests, ProxyOnEventNullProxyReturnsError) {
  rmi_error err =
      rmi_proxy_on_event(nullptr, H("evt"), noop_proxy_event_handler, nullptr);
  EXPECT_EQ(err, RMI_ERR_NULL);
}

TEST(ErrorHandlingTests, ProxyOnEventNullHandlerReturnsError) {
  rmi_error err = rmi_proxy_on_event(nullptr, H("evt"), nullptr, nullptr);
  EXPECT_EQ(err, RMI_ERR_NULL);
}

TEST(ErrorHandlingTests, PtyClientRunNullCallbacksReturnsError) {
  rmi_error err = rmi_pty_client_run(0, nullptr, nullptr);
  EXPECT_EQ(err, RMI_ERR_NULL);
}

TEST(ErrorHandlingTests, PtyClientRunMissingSessionCallbackReturnsError) {
  rmi_pty_client_callbacks callbacks{};
  callbacks.on_error = noop_pty_client_on_error;
  callbacks.user = nullptr;

  rmi_error err = rmi_pty_client_run(0, nullptr, &callbacks);
  EXPECT_EQ(err, RMI_ERR_NULL);
}

TEST(ErrorHandlingTests, PtyClientRunNullArgvWithArgcReturnsError) {
  rmi_pty_client_callbacks callbacks{};
  callbacks.on_session = noop_pty_client_on_session;
  callbacks.on_error = noop_pty_client_on_error;
  callbacks.user = nullptr;

  rmi_error err = rmi_pty_client_run(1, nullptr, &callbacks);
  EXPECT_EQ(err, RMI_ERR_NULL);
}

TEST(ErrorHandlingTests, PtyServerRunNullCallbacksReturnsError) {
  rmi_error err = rmi_pty_server_run(0, nullptr, nullptr);
  EXPECT_EQ(err, RMI_ERR_NULL);
}

TEST(ErrorHandlingTests, PtyServerRunMissingRegisterCallbackReturnsError) {
  rmi_pty_server_callbacks callbacks{};
  callbacks.on_error = noop_pty_server_on_error;
  callbacks.user = nullptr;

  rmi_error err = rmi_pty_server_run(0, nullptr, &callbacks);
  EXPECT_EQ(err, RMI_ERR_NULL);
}

TEST(ErrorHandlingTests, PtyServerRunNullArgvWithArgcReturnsError) {
  rmi_pty_server_callbacks callbacks{};
  callbacks.register_classes = noop_pty_server_register_classes;
  callbacks.on_error = noop_pty_server_on_error;
  callbacks.user = nullptr;

  rmi_error err = rmi_pty_server_run(1, nullptr, &callbacks);
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

  rmi_error err = rmi_server_listen(server, params);
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
    // With null client/proxy, should fail with RMI_ERR_NULL, not timeout.
    rmi_error err =
        rmi_proxy_call(nullptr, nullptr, H("m"), nullptr, &dummy_result, tv);
    EXPECT_EQ(err, RMI_ERR_NULL);
  }
}

TEST(TimeoutTests, ProxyClearWithTimeoutMs) {
  int64_t timeout_vals[] = {-1, 0, 1000, 5000};
  for (int64_t tv : timeout_vals) {
    rmi_error err = rmi_proxy_clear(nullptr, nullptr, tv);
    EXPECT_EQ(err, RMI_ERR_NULL);
  }
}

TEST(TimeoutTests, ProxySetWithTimeoutMs) {
  int64_t timeout_vals[] = {-1, 0, 1000, 5000};
  for (int64_t tv : timeout_vals) {
    rmi_error err = rmi_proxy_set(nullptr, nullptr, nullptr, tv);
    EXPECT_EQ(err, RMI_ERR_NULL);
  }
}

TEST(TimeoutTests, ProxyGetWithTimeoutMs) {
  bison_handle dummy_result = nullptr;
  int64_t timeout_vals[] = {-1, 0, 1000, 5000};
  for (int64_t tv : timeout_vals) {
    rmi_error err = rmi_proxy_get(nullptr, nullptr, nullptr, &dummy_result, tv);
    EXPECT_EQ(err, RMI_ERR_NULL);
  }
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
    bool ok = bison_add_class(0, proto) == BISON_OK;
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
  EXPECT_EQ(rmi_client_describe(sa, 0, &desc), RMI_OK);
  EXPECT_NE(desc, nullptr);
  bison_release(desc);
}

TEST_F(StandaloneAbiTests, InstantiateAndProxyGetField) {
  ASSERT_TRUE(register_counter_class());

  ScopedClientHandle sa{rmi_standalone_create()};
  ASSERT_NE(sa.h, nullptr);

  rmi_proxy_handle proxy = nullptr;
  ASSERT_EQ(
      rmi_client_instantiate(sa, bison_key("StandaloneCounter"), nullptr, &proxy),
      RMI_OK);
  ASSERT_NE(proxy, nullptr);

  bison_handle snap = nullptr;
  ASSERT_EQ(rmi_proxy_get(sa, proxy, nullptr, &snap, -1), RMI_OK);
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
  ASSERT_EQ(
      rmi_client_instantiate(sa, bison_key("StandaloneCounter"), nullptr, &proxy),
      RMI_OK);
  ASSERT_NE(proxy, nullptr);

  // Set value to 42.
  ScopedBisonHandle fields{bison_create(0)};
  ASSERT_NE(fields.h, nullptr);
  ASSERT_EQ(bison_set_int(fields, bison_key("value"), 42), BISON_OK);
  EXPECT_EQ(rmi_proxy_set(sa, proxy, fields, -1), RMI_OK);

  // Read it back.
  bison_handle snap = nullptr;
  ASSERT_EQ(rmi_proxy_get(sa, proxy, nullptr, &snap, -1), RMI_OK);
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
  ASSERT_EQ(
      rmi_client_instantiate(sa, bison_key("StandaloneCounter"), nullptr, &proxy),
      RMI_OK);

  ScopedBisonHandle fields{bison_create(0)};
  bison_set_int(fields, bison_key("value"), 99);
  EXPECT_EQ(rmi_proxy_set(sa, proxy, fields, -1), RMI_OK);

  EXPECT_EQ(rmi_proxy_clear(sa, proxy, -1), RMI_OK);

  bison_handle snap = nullptr;
  ASSERT_EQ(rmi_proxy_get(sa, proxy, nullptr, &snap, -1), RMI_OK);
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
  rmi_error err =
      rmi_client_instantiate(sa, bison_key("NoSuchClass"), nullptr, &proxy);
  EXPECT_NE(err, RMI_OK);
  EXPECT_EQ(proxy, nullptr);
}

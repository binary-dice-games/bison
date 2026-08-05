// MIT License © 2025 Binary Dice Games
// Google Test suite for the header-only C++ ABI binding's RMI API
// (bindings/cpp/include/bison/rmi.hpp), wrapping rmi_c.h / bison_abi.

#include "bison/rmi.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

using namespace bdg::bison::abi;

// bdg::bison::abi::key_t must stay explicitly qualified below -- see
// dynamic_tests.cpp's identical note (glibc's <sys/types.h> also defines a
// global `key_t`).
using bison_key_t = bdg::bison::abi::key_t;

namespace {

void register_calculator() {
  dynamic proto{"Calculator"_key};
  proto.addMethod("add"_key, [](dynamic&, const dynamic& params, dynamic& result) {
    result["result"_key] = params["a"_key].as<float>() + params["b"_key].as<float>();
  });
  dynamic::addClass(proto);
}

class RmiStandaloneTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dynamic::clear_registry();
    register_calculator();
  }
  void TearDown() override {
    dynamic::clear_registry();
  }
};

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// Standalone (in-process) client/proxy
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(RmiStandaloneTest, InstantiateAndCall) {
  auto client = rmi::client::standalone();
  client.connect();

  auto calc = client.instantiate("Calculator"_key);
  dynamic args;
  args["a"_key] = 10.0f;
  args["b"_key] = 3.0f;
  dynamic result = calc.call("add"_key, args);
  EXPECT_FLOAT_EQ(result["result"_key].as<float>(), 13.0f);
}

TEST_F(RmiStandaloneTest, SetAndGet) {
  auto client = rmi::client::standalone();
  client.connect();
  auto calc = client.instantiate("Calculator"_key);

  dynamic fields;
  fields["extra"_key] = int32_t{7};
  calc.set(fields);

  dynamic snapshot = calc.get();
  EXPECT_EQ(snapshot["extra"_key].as<int32_t>(), 7);
}

TEST_F(RmiStandaloneTest, GetWithProjection) {
  auto client = rmi::client::standalone();
  client.connect();
  auto calc = client.instantiate("Calculator"_key);

  dynamic fields;
  fields["a"_key] = 1.0f;
  fields["b"_key] = 2.0f;
  calc.set(fields);

  dynamic projection;
  projection["a"_key] = true;
  dynamic projected = calc.get(projection);
  EXPECT_FLOAT_EQ(projected["a"_key].as<float>(), 1.0f);
}

TEST_F(RmiStandaloneTest, Clear) {
  auto client = rmi::client::standalone();
  client.connect();
  auto calc = client.instantiate("Calculator"_key);

  dynamic fields;
  fields["extra"_key] = int32_t{7};
  calc.set(fields);
  calc.clear();

  // After clear(), the explicitly-set field is gone; auto-vivification on
  // read yields the zero default rather than 7.
  dynamic snapshot = calc.get();
  EXPECT_EQ(snapshot["extra"_key].as<int32_t>(), 0);
}

TEST_F(RmiStandaloneTest, AsyncInstantiateAndCall) {
  auto client = rmi::client::standalone();
  client.connect();

  auto future = client.instantiate_async("Calculator"_key);
  auto calc = future.get_proxy();

  dynamic args;
  args["a"_key] = 4.0f;
  args["b"_key] = 5.0f;
  auto call_future = calc.call_async("add"_key, args);
  call_future.wait();
  dynamic result = call_future.get_dynamic();
  EXPECT_FLOAT_EQ(result["result"_key].as<float>(), 9.0f);
}

TEST_F(RmiStandaloneTest, ProxyValidBecomesFalseAfterDestroy) {
  auto client = rmi::client::standalone();
  client.connect();
  auto calc = client.instantiate("Calculator"_key);
  EXPECT_TRUE(calc.valid());
  calc.destroy();
  EXPECT_FALSE(calc.valid());
}

TEST_F(RmiStandaloneTest, MoveTransfersOwnership) {
  auto client = rmi::client::standalone();
  client.connect();
  auto calc = client.instantiate("Calculator"_key);
  auto moved = std::move(calc);
  EXPECT_FALSE(calc.valid());
  EXPECT_TRUE(moved.valid());
}

TEST_F(RmiStandaloneTest, DescribeReturnsClassMetadata) {
  auto client = rmi::client::standalone();
  client.connect();
  dynamic desc = client.describe();
  EXPECT_TRUE(desc.valid());
}

TEST_F(RmiStandaloneTest, ClientDestroyHelper) {
  auto client = rmi::client::standalone();
  client.connect();
  auto calc = client.instantiate("Calculator"_key);
  client.destroy(std::move(calc));
  EXPECT_FALSE(calc.valid());
}

// ═════════════════════════════════════════════════════════════════════════════
// TCP client/server round trip
// ═════════════════════════════════════════════════════════════════════════════

namespace {

// Mirrors tests/rmi_tests.cpp's make_socket_server_transport() retry-over-
// ports pattern, to avoid flakiness from a fixed port already being in use.
// `configure`, if given, runs after the server is created but before
// `listen()` -- e.g. to call `set_auth()`, which must precede `listen()`.
std::pair<rmi::server, uint16_t> make_tcp_server(std::function<void(rmi::server&)> configure = {}) {
  static std::atomic<uint16_t> next_port{29500};
  for (int attempt = 0; attempt < 64; ++attempt) {
    uint16_t port = next_port.fetch_add(1);
    try {
      rmi::server server = rmi::server::tcp("127.0.0.1", port);
      if (configure)
        configure(server);
      server.listen();
      return {std::move(server), port};
    } catch (const std::exception&) {
    }
  }
  throw std::runtime_error("unable to allocate a TCP test port");
}

} // namespace

TEST(RmiTcp, ClientServerRoundTrip) {
  dynamic::clear_registry();
  register_calculator();

  auto [server, port] = make_tcp_server();

  rmi::client client = rmi::client::tcp("127.0.0.1", port);
  client.connect();
  auto calc = client.instantiate("Calculator"_key);

  dynamic args;
  args["a"_key] = 20.0f;
  args["b"_key] = 22.0f;
  dynamic result = calc.call("add"_key, args);
  EXPECT_FLOAT_EQ(result["result"_key].as<float>(), 42.0f);

  calc.destroy();
  client.disconnect();
  server.stop();
  dynamic::clear_registry();
}

// ═════════════════════════════════════════════════════════════════════════════
// server::set_auth()
// ═════════════════════════════════════════════════════════════════════════════

TEST(RmiAuth, AcceptingCallbackReceivesPayloadAndSetsIdentity) {
  std::string seen_username;
  auto [server, port] = make_tcp_server([&](rmi::server& s) {
    s.set_auth([&](const dynamic& payload, std::string& out_identity) {
      seen_username = payload["username"_key].as<std::string>();
      out_identity = "alice-id";
      return true;
    });
  });

  rmi::client client = rmi::client::tcp("127.0.0.1", port);
  dynamic params;
  params["username"_key] = std::string{"alice"};
  client.connect(params);

  EXPECT_EQ(seen_username, "alice");

  client.disconnect();
  server.stop();
}

TEST(RmiAuth, RejectingCallbackFailsConnect) {
  auto [server, port] = make_tcp_server([](rmi::server& s) {
    s.set_auth([](const dynamic&, std::string&) { return false; });
  });

  rmi::client client = rmi::client::tcp("127.0.0.1", port);
  EXPECT_THROW(client.connect(), std::exception);

  server.stop();
}

TEST(RmiAuth, SetAuthAfterListenThrows) {
  auto [server, port] = make_tcp_server();
  (void)port;

  EXPECT_THROW(server.set_auth([](const dynamic&, std::string&) { return true; }), std::exception);

  server.stop();
}

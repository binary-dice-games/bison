// MIT License © 2025 Binary Dice Games
// Bridge integration tests.

#include "src/rmi/rmi.hpp"
#include "src/rmi/shared/schemas.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace bdg::bison;
using namespace bdg::bison::rmi;
using namespace bdg::bison::rmi::shared;
using namespace bdg::bison::rmi::shared::constants;
using namespace bdg::bison::rmi::transport;

using bison_key_t = bdg::bison::key_t;

static void clearClassRegistry() {
  dynamic::getRegistry().wlock()->clear();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test fixture: one upstream server + one bridge + convenience client factory
// ─────────────────────────────────────────────────────────────────────────────

class BridgeTest : public ::testing::Test {
 protected:
  // Upstream (the "wish server" side).
  memory_server_transport upstream_transport_;
  std::unique_ptr<server> upstream_srv_;

  // Bridge downstream (connects downstream test clients).
  memory_server_transport downstream_transport_;
  std::unique_ptr<bridge> br_;

  void SetUp() override {
    clearClassRegistry();
    register_all_schemas();

    // Start the upstream server.
    upstream_srv_ = std::make_unique<server>(upstream_transport_);
    upstream_srv_->listen();

    // Construct the bridge: upstream transport connects to upstream_transport_,
    // downstream clients connect via downstream_transport_.
    br_ = std::make_unique<bridge>(
        downstream_transport_, std::make_unique<memory_client_transport>(upstream_transport_.connect()));
    br_->start();
  }

  void TearDown() override {
    br_->stop();
    br_.reset();
    upstream_srv_->stop();
    upstream_srv_.reset();
  }

  client make_client() {
    return client{downstream_transport_.connect()};
  }
};

// ═════════════════════════════════════════════════════════════════════════════
// 1. Connect / disconnect
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(BridgeTest, DownstreamClientConnectsAndDisconnects) {
  auto c = make_client();
  EXPECT_NO_THROW(c.connect());
  EXPECT_NO_THROW(c.disconnect());
}

TEST_F(BridgeTest, TwoClientsConnectAndDisconnect) {
  auto c1 = make_client();
  auto c2 = make_client();
  c1.connect();
  c2.connect();
  c1.disconnect();
  c2.disconnect();
}

// ═════════════════════════════════════════════════════════════════════════════
// 2. Object instantiation forwarded to upstream
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(BridgeTest, InstantiateCreatesObjectOnUpstream) {
  auto proto = dynamic_ptr{"Box"_key, {{"w"_key, int32_t{0}}}};
  dynamic::addClass(0U, proto, 0U);

  auto c = make_client();
  c.connect();

  auto proxy = c.instantiate(0U, "Box"_key).get();
  EXPECT_TRUE(proxy.valid());

  // Upstream server should have one object in the session context.
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  {
    auto lp = upstream_srv_->session_contexts().rlock();
    bool found = false;
    for (auto& [id, holder] : *lp) {
      auto clp = holder->rlock();
      if (!(*clp)->objects.empty()) {
        found = true;
        break;
      }
    }
    EXPECT_TRUE(found);
  }

  c.destroy(std::move(proxy));
  c.disconnect();
}

TEST_F(BridgeTest, DestroyRemovesObjectFromUpstream) {
  auto proto = dynamic_ptr{"Box"_key, {{"w"_key, int32_t{0}}}};
  dynamic::addClass(0U, proto, 0U);

  auto c = make_client();
  c.connect();

  auto proxy = c.instantiate(0U, "Box"_key).get();
  c.destroy(std::move(proxy));
  std::this_thread::sleep_for(std::chrono::milliseconds{80});

  // After destroy the upstream should have no objects.
  {
    auto lp = upstream_srv_->session_contexts().rlock();
    for (auto& [id, holder] : *lp) {
      auto clp = holder->rlock();
      EXPECT_TRUE((*clp)->objects.empty()) << "upstream session still holds objects after destroy";
    }
  }

  c.disconnect();
}

// ═════════════════════════════════════════════════════════════════════════════
// 3. SET / GET forwarded
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(BridgeTest, SetAndGetForwardedToUpstream) {
  auto proto = dynamic_ptr{"Counter"_key, {{"n"_key, int32_t{0}}}};
  dynamic::addClass(0U, proto, 0U);

  auto c = make_client();
  c.connect();

  auto proxy = c.instantiate(0U, "Counter"_key).get();

  dynamic fields;
  fields["n"_key] = int32_t{42};
  proxy.set(std::move(fields)).get();

  dynamic result = proxy.get().get();
  EXPECT_EQ(result.as<int32_t>("n"_key), int32_t{42});

  c.destroy(std::move(proxy));
  c.disconnect();
}

// ═════════════════════════════════════════════════════════════════════════════
// 4. Method call forwarded via CALL_FALLBACK
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(BridgeTest, MethodCallForwardedToUpstream) {
  auto proto = dynamic_ptr{"Adder"_key, {{"val"_key, int32_t{0}}}};
  proto->addMethod("add"_key, method{[](dynamic& self, const dynamic& p) {
                     int32_t x = self["val"_key];
                     int32_t y = p.as<int32_t>("y"_key);
                     self["val"_key] = int32_t{x + y};
                     return dynamic{};
                   }});
  dynamic::addClass(0U, proto, 0U);

  auto c = make_client();
  c.connect();

  auto proxy = c.instantiate(0U, "Adder"_key).get();

  // Set initial value.
  dynamic init;
  init["val"_key] = int32_t{10};
  proxy.set(std::move(init)).get();

  // Call add(y=5) via bridge — uses CALL_FALLBACK.
  dynamic call_params;
  call_params["y"_key] = int32_t{5};
  proxy.call("add"_key, std::move(call_params)).get();

  // GET should reflect the updated value.
  dynamic result = proxy.get().get();
  EXPECT_EQ(result.as<int32_t>("val"_key), int32_t{15});

  c.destroy(std::move(proxy));
  c.disconnect();
}

// ═════════════════════════════════════════════════════════════════════════════
// 5. Events routed from upstream to the correct downstream client
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(BridgeTest, EventRoutedToCorrectClient) {
  // A class whose "fire" method emits an event back to the caller's session.
  auto proto = dynamic_ptr{"Emitter"_key, {}};
  proto->addMethod("fire"_key, method{[](dynamic& self, const dynamic& /*p*/) {
                     // Retrieve emit callback from userdata.
                     struct emit_ud : userdata {
                       std::function<void(bison_key_t, bison_key_t, dynamic)> fn;
                     };
                     auto ud = std::dynamic_pointer_cast<emit_ud>(self.getUserdata());
                     if (ud && ud->fn) {
                       dynamic ev;
                       ev["tick"_key] = int32_t{1};
                       ud->fn({}, "onTick"_key, std::move(ev));
                     }
                     return dynamic{};
                   }});
  dynamic::addClass(0U, proto, 0U);

  // We need access to the upstream server's on_create_object to stash
  // emit_event.  Use a subclassed server via a fresh fixture.
  // For simplicity, test event routing via the known envelope path by
  // directly using the upstream emit_event function.
  //
  // Instead, test that events arrive only at the subscribing client.
  // We use the upstream server's session_contexts to reach emit_event.

  std::atomic<int> c1_count{0};
  std::atomic<int> c2_count{0};

  auto c1 = make_client();
  auto c2 = make_client();
  c1.connect();
  c2.connect();

  auto proxy1 = c1.instantiate(0U, "Emitter"_key).get();
  auto proxy2 = c2.instantiate(0U, "Emitter"_key).get();

  proxy1.onEvent("onTick"_key, [&](dynamic) { ++c1_count; });
  proxy2.onEvent("onTick"_key, [&](dynamic) { ++c2_count; });

  // Wait for bridge to set up the upstream mappings.
  std::this_thread::sleep_for(std::chrono::milliseconds{50});

  // Emit an event directly from the upstream server for the upstream object
  // that corresponds to c1's proxy1.  Find it via upstream session_contexts.
  {
    auto lp = upstream_srv_->session_contexts().rlock();
    for (auto& [id, holder] : *lp) {
      auto clp = holder->rlock();
      if (!(*clp)->objects.empty() && (*clp)->emit_event) {
        // There are two upstream objects (one per downstream proxy).
        // Emit for the first object we find.
        auto it = (*clp)->objects.begin();
        (*clp)->emit_event(it->first, "onTick"_key, dynamic{});
        break;
      }
    }
  }

  std::this_thread::sleep_for(std::chrono::milliseconds{80});

  // Exactly one client should have received the event.
  EXPECT_EQ(c1_count.load() + c2_count.load(), 1);

  c1.destroy(std::move(proxy1));
  c2.destroy(std::move(proxy2));
  c1.disconnect();
  c2.disconnect();
}

// ═════════════════════════════════════════════════════════════════════════════
// 6. Cross-client isolation: client B cannot use client A's object ID
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(BridgeTest, CrossClientIsolation) {
  auto proto = dynamic_ptr{"Widget"_key, {{"x"_key, int32_t{0}}}};
  dynamic::addClass(0U, proto, 0U);

  auto c1 = make_client();
  auto c2 = make_client();
  c1.connect();
  c2.connect();

  auto proxy1 = c1.instantiate(0U, "Widget"_key).get();
  // Attempt to use proxy1's object ID from c2 (different session).
  // make_proxy creates a proxy without going through instantiate, so the
  // server worker for c2 won't find the object ID in c2's ctx.objects.
  auto fake_proxy = c2.make_proxy(proxy1.object_id());

  // A GET on a non-existent object ID should throw (future holds exception).
  bool threw = false;
  try {
    fake_proxy.get().get();
  } catch (...) {
    threw = true;
  }
  EXPECT_TRUE(threw) << "expected ERR_OBJECT_NOT_FOUND for cross-client access";

  c1.destroy(std::move(proxy1));
  c1.disconnect();
  c2.disconnect();
}

// ═════════════════════════════════════════════════════════════════════════════
// 7. OP_CLEAR re-installs the forwarding proxy
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(BridgeTest, ClearAndSubsequentSetGetWork) {
  auto proto = dynamic_ptr{"Box"_key, {{"w"_key, int32_t{0}}}};
  dynamic::addClass(0U, proto, 0U);

  auto c = make_client();
  c.connect();

  auto proxy = c.instantiate(0U, "Box"_key).get();

  // Set, then clear, then set again.
  dynamic f1;
  f1["w"_key] = int32_t{77};
  proxy.set(std::move(f1)).get();

  proxy.clear().get();

  dynamic f2;
  f2["w"_key] = int32_t{99};
  proxy.set(std::move(f2)).get();

  dynamic result = proxy.get().get();
  EXPECT_EQ(result.as<int32_t>("w"_key), int32_t{99});

  c.destroy(std::move(proxy));
  c.disconnect();
}

// ═════════════════════════════════════════════════════════════════════════════
// 8. Session teardown destroys all upstream objects
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(BridgeTest, DisconnectDestroysUpstreamObjects) {
  std::atomic<int> destruct_count{0};
  auto proto = dynamic_ptr{"Tracked"_key, {}};
  proto->addMethod(HOOK_DESTRUCT, method{[&destruct_count](dynamic& /*self*/, const dynamic&) {
                     ++destruct_count;
                     return dynamic{};
                   }});
  dynamic::addClass(0U, proto, 0U);

  auto c = make_client();
  c.connect();

  auto p1 = c.instantiate(0U, "Tracked"_key).get();
  auto p2 = c.instantiate(0U, "Tracked"_key).get();

  // Disconnect without explicit destroy — bridge teardown should destroy both.
  c.disconnect();
  std::this_thread::sleep_for(std::chrono::milliseconds{120});

  EXPECT_EQ(destruct_count.load(), 2) << "expected both upstream objects destroyed on disconnect";
}

// ═════════════════════════════════════════════════════════════════════════════
// 9. CALL_FALLBACK constant is backward-compatible
// ═════════════════════════════════════════════════════════════════════════════

TEST(CallFallback, KnownMethodTakesPriorityOverFallback) {
  clearClassRegistry();

  std::atomic<int> fallback_calls{0};
  std::atomic<int> known_calls{0};

  auto proto = dynamic_ptr{"Dual"_key, {}};
  proto->addMethod("knownMethod"_key, method{[&known_calls](dynamic&, const dynamic&) {
                     ++known_calls;
                     return dynamic{};
                   }});
  proto->addMethod(bison_key_t{dynamic::CALL_FALLBACK}, method{[&fallback_calls](dynamic&, const dynamic&) {
                     ++fallback_calls;
                     return dynamic{};
                   }});
  dynamic::addClass(0U, proto, 0U);

  memory_server_transport t;
  server srv{t};
  srv.listen();

  client c{t.connect()};
  c.connect();

  auto proxy = c.instantiate(0U, "Dual"_key).get();

  // Known method goes through the direct dispatch path.
  proxy.call("knownMethod"_key, dynamic{}).get();
  EXPECT_EQ(known_calls.load(), 1);
  EXPECT_EQ(fallback_calls.load(), 0);

  // Unknown method should trigger CALL_FALLBACK.
  proxy.call("unknownMethod"_key, dynamic{}).get();
  EXPECT_EQ(known_calls.load(), 1);
  EXPECT_EQ(fallback_calls.load(), 1);

  c.destroy(std::move(proxy));
  c.disconnect();
  srv.stop();
}

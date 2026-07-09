// MIT License © 2025 Binary Dice Games
// Tests for src/app/bridge/bridge_app.hpp

#include "src/app/bridge/bridge_app.hpp"
#include "src/rmi/rmi.hpp"

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

// bridge_app.cpp / transport_flags.cpp / upstream_transport_flags.cpp DECLARE
// these flags for CLI-driven binaries to DEFINE; this test executable calls
// run_with_transport() directly (bypassing run()'s CLI parsing) but still
// links the object code that references them, so they must be defined here.
// Mirrors src/app/abi_flags.cpp's rationale for bison_abi.dll.
DEFINE_string(transport, "tcp", "test default");
DEFINE_string(host, "127.0.0.1", "test default");
DEFINE_int32(port, 7070, "test default");
DEFINE_string(name, "", "test default");
DEFINE_string(cmd, "", "test default");
DEFINE_int32(timeout, 30000, "test default");
DEFINE_bool(debugger, false, "test default");
DEFINE_string(upstream_transport, "tcp", "test default");
DEFINE_string(upstream_host, "127.0.0.1", "test default");
DEFINE_int32(upstream_port, 7070, "test default");
DEFINE_string(upstream_name, "", "test default");

using namespace bdg::bison;
using namespace bdg::bison::rmi;
using namespace bdg::bison::rmi::shared::constants;
using namespace bdg::bison::rmi::transport;

namespace {

static void clearClassRegistry() {
  dynamic::getRegistry().wlock()->clear();
}

class test_bridge_app : public app::bridge_app {
 public:
  using app::bridge_app::run_with_transport;

  std::atomic<int> client_connected_count{0};
  std::atomic<int> client_disconnected_count{0};

 protected:
  void on_client_connected(rmi::context& ctx) const override {
    (void)ctx;
    const_cast<test_bridge_app*>(this)->client_connected_count++;
  }

  void on_client_disconnected(rmi::context& ctx) const override {
    (void)ctx;
    const_cast<test_bridge_app*>(this)->client_disconnected_count++;
  }
};

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
  manual_shutdown_gate gate;

  std::thread bridge_thread([&] {
    app.run_with_transport(
        downstream_transport_,
        std::make_unique<memory_client_transport>(upstream_transport_.connect()),
        [&] { gate.wait(); });
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

  gate.stop();
  bridge_thread.join();
}

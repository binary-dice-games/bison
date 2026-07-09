// MIT License © 2025 Binary Dice Games
// Tests for src/app/standalone/standalone_app.hpp

#include "src/app/standalone/standalone_app.hpp"

#include <gflags/gflags.h>
#include <gtest/gtest.h>

// standalone_app.cpp DECLAREs --debugger for --debugger-attach support; the
// real binary that hosts it (an example, or wish's own CLI) normally defines
// it, so this test executable must provide the definition itself, mirroring
// src/app/abi_flags.cpp's rationale for bison_abi.dll.
DEFINE_bool(debugger, false, "Wait for debugger attachment before starting (test default)");

using namespace bdg::bison;
using namespace bdg::bison::rmi;

namespace {

static void clearClassRegistry() {
  dynamic::getRegistry().wlock()->clear();
}

class test_standalone_app : public app::standalone_app {
 public:
  bool register_classes_called = false;
  bool on_session_called = false;
  bool session_created_called = false;
  bool session_destroyed_called = false;
  int session_return_value = 0;

 protected:
  void register_classes() override {
    register_classes_called = true;
    auto proto = dynamic_ptr{"Counter"_key, {{"value"_key, int32_t{0}}}};
    dynamic::addClass(0U, proto);
  }

  int on_session(standalone& sa) override {
    on_session_called = true;
    auto proxy = sa.instantiate(0U, "Counter"_key).get();
    bool valid = proxy.valid();
    sa.destroy(std::move(proxy));
    return valid ? session_return_value : 1;
  }

  void on_session_created(rmi::context& ctx) const override {
    const_cast<test_standalone_app*>(this)->session_created_called = true;
    (void)ctx;
  }

  void on_session_destroyed(rmi::context& ctx) const override {
    const_cast<test_standalone_app*>(this)->session_destroyed_called = true;
    (void)ctx;
  }
};

class StandaloneAppTest : public ::testing::Test {
 protected:
  void SetUp() override {
    clearClassRegistry();
  }
};

} // namespace

TEST_F(StandaloneAppTest, RunsRegisterClassesThenSessionThenHooks) {
  test_standalone_app app;
  char program_name[] = "standalone_app_test";
  char* argv[] = {program_name};
  int argc = 1;

  int result = app.run(argc, argv);

  EXPECT_EQ(result, 0);
  EXPECT_TRUE(app.register_classes_called);
  EXPECT_TRUE(app.on_session_called);
  EXPECT_TRUE(app.session_created_called);
  EXPECT_TRUE(app.session_destroyed_called);
}

TEST_F(StandaloneAppTest, ReturnsOnSessionResult) {
  test_standalone_app app;
  app.session_return_value = 42;
  char program_name[] = "standalone_app_test";
  char* argv[] = {program_name};
  int argc = 1;

  EXPECT_EQ(app.run(argc, argv), 42);
}

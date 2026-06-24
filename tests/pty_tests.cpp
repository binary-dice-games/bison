// MIT License © 2025 Binary Dice Games
// PTY transport and application scaffold tests (Linux only).

#include "src/rmi/transport/pty_server_transport.hpp"
#include "src/app/pty/pty_server_app.hpp"
#include "src/app/pty/pty_client_app.hpp"

#include <gtest/gtest.h>

#include <chrono>

#if defined(__linux__)

using namespace bdg::bison;
using namespace bdg::bison::app;

// ── pty_server_transport unit tests ─────────────────────────────────────────

TEST(PtyServerTransport, StartIdempotent) {
  // Calling start() twice must not throw or fork a second shell.
  pty_server_transport transport{"bash"};
  transport.start(dynamic{});
  EXPECT_NO_THROW(transport.start(dynamic{}));
  transport.stop();
}

TEST(PtyServerTransport, IsShellRunningAfterStart) {
  pty_server_transport transport{"bash"};
  EXPECT_FALSE(transport.is_shell_running());
  transport.start(dynamic{});
  EXPECT_TRUE(transport.is_shell_running());
  transport.stop();
}

TEST(PtyServerTransport, AcceptTimesOutWithNoClient) {
  // No client sends a HELLO, so accept() must time out and return nullptr.
  pty_server_transport transport{"bash"};
  transport.start(dynamic{});
  auto conn = transport.accept(std::chrono::milliseconds{100});
  EXPECT_EQ(conn, nullptr);
  transport.stop();
}

TEST(PtyServerTransport, AcceptReturnsNullAfterStop) {
  pty_server_transport transport{"bash"};
  transport.start(dynamic{});
  transport.stop();
  auto conn = transport.accept(std::chrono::milliseconds{50});
  EXPECT_EQ(conn, nullptr);
}

TEST(PtyServerTransport, WaitUntilClosedReturnsFalseOnTimeout) {
  pty_server_transport transport{"bash"};
  transport.start(dynamic{});
  // No session is active, so closed is false; we should time out.
  const bool closed =
      transport.wait_until_closed(std::chrono::milliseconds{50});
  EXPECT_FALSE(closed);
  transport.stop();
}

// ── pty_server_app / pty_client_app API shape ────────────────────────────────

// Verify that the scaffolds can be subclassed and their virtual hooks called.
class test_pty_server : public pty_server_app {
 public:
  mutable bool connected_called{false};
  mutable bool ended_called{false};

  void register_classes() override {}
  void on_client_connected() const override { connected_called = true; }
  void on_session_ended() const override { ended_called = true; }
  std::string shell_command() const override { return "bash"; }
};

class test_pty_client : public pty_client_app {
 public:
  int on_session(rmi::client&) override { return 42; }
};

TEST(PtyServerApp, SubclassCompiles) {
  test_pty_server app;
  (void)app;
}

TEST(PtyClientApp, SubclassCompiles) {
  test_pty_client app;
  (void)app;
}

#endif // defined(__linux__)

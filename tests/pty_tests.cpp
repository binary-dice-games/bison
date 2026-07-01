// MIT License © 2025 Binary Dice Games
// PTY transport integration tests.
//
// These tests run only on Linux because forkpty(3) is Linux-specific.
// The Windows ConPTY tests would require a different test infrastructure
// (a child process built with the test binary itself).
#ifdef __linux__

#include "src/rmi/pty/pty_process.hpp"
#include "src/rmi/transport/pty_server_transport.hpp"
#include "src/rmi/transport/pty_client_transport.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

using namespace bdg::bison::rmi::pty;
using namespace bdg::bison::rmi::transport;

// ═════════════════════════════════════════════════════════════════════════════
// pty_process — basic spawn and wait
// ═════════════════════════════════════════════════════════════════════════════

TEST(PtyProcess, SpawnAndWaitSuccess) {
  pty_process p{pty_config{.cmd = "/bin/sh", .args = {"-c", "exit 0"}}};
  EXPECT_EQ(p.wait(), 0);
}

TEST(PtyProcess, SpawnAndWaitNonZero) {
  pty_process p{pty_config{.cmd = "/bin/sh", .args = {"-c", "exit 42"}}};
  EXPECT_EQ(p.wait(), 42);
}

TEST(PtyProcess, MasterFdIsValid) {
  pty_process p{pty_config{.cmd = "/bin/sh", .args = {"-c", "exit 0"}}};
  EXPECT_GE(p.master_fd(), 0);
  p.wait();
}

TEST(PtyProcess, IsattyTrueInChild) {
  // "test -t 0" exits 0 if stdin is a terminal (isatty(0) == true).
  pty_process p{pty_config{.cmd = "/bin/sh", .args = {"-c", "test -t 0"}}};
  EXPECT_EQ(p.wait(), 0);
}

TEST(PtyProcess, MoveConstruct) {
  pty_process p1{pty_config{.cmd = "/bin/sh", .args = {"-c", "exit 0"}}};
  const int fd = p1.master_fd();
  pty_process p2{std::move(p1)};
  EXPECT_EQ(p2.master_fd(), fd);
  EXPECT_EQ(p2.wait(), 0);
}

TEST(PtyProcess, Terminate) {
  // Launch a process that sleeps indefinitely; terminate it.
  pty_process p{pty_config{.cmd = "/bin/sleep", .args = {"60"}}};
  p.terminate();
  const int code = p.wait();
  EXPECT_NE(code, 0); // killed by signal → negative exit code or non-zero
}

// ═════════════════════════════════════════════════════════════════════════════
// pty_server_transport — DCS frame round-trip via cat
// ═════════════════════════════════════════════════════════════════════════════

TEST(PtyServerTransport, StartAndAccept) {
  pty_server_transport srv{pty_config{.cmd = "/bin/cat"}};
  srv.start(bdg::bison::dynamic{});
  auto conn = srv.accept(std::chrono::milliseconds{1000});
  ASSERT_NE(conn, nullptr);
  EXPECT_FALSE(conn->is_closed());
  conn->close();
}

TEST(PtyServerTransport, DcsFrameRoundTrip) {
  // Spawn cat with unbuffered output so DCS frame bytes are echoed immediately.
  // stdbuf -o0 disables cat's line-buffered stdio on the PTY slave stdout.
  pty_server_transport srv{
      pty_config{.cmd = "/usr/bin/stdbuf", .args = {"-o0", "cat"}}};
  srv.start(bdg::bison::dynamic{});
  auto conn = srv.accept(std::chrono::milliseconds{1000});
  ASSERT_NE(conn, nullptr);

  // The server connection sets raw mode on construction (suppresses PTY echo).
  const bdg::bison::buffer payload{'H', 'e', 'l', 'l', 'o', ' ', 'P', 'T', 'Y'};
  conn->send(payload);

  // stdbuf -o0 ensures cat echoes without buffering.
  bdg::bison::buffer received;
  const bool ok = conn->receive(received, std::chrono::milliseconds{3000});
  EXPECT_TRUE(ok);
  if (ok)
    EXPECT_EQ(received, payload);

  conn->close();
}

TEST(PtyServerTransport, StopBeforeAccept) {
  pty_server_transport srv{pty_config{.cmd = "/bin/cat"}};
  srv.start(bdg::bison::dynamic{});
  srv.stop();
  auto conn = srv.accept(std::chrono::milliseconds{100});
  EXPECT_EQ(conn, nullptr);
}

TEST(PtyServerTransport, AcceptReturnNullptrOnSecondCall) {
  pty_server_transport srv{pty_config{.cmd = "/bin/cat"}};
  srv.start(bdg::bison::dynamic{});
  auto c1 = srv.accept(std::chrono::milliseconds{1000});
  ASSERT_NE(c1, nullptr);
  auto c2 = srv.accept(std::chrono::milliseconds{100});
  EXPECT_EQ(c2, nullptr);
  c1->close();
}

#endif // __linux__

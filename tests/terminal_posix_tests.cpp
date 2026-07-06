// MIT License © 2025 Binary Dice Games
// Tests for bdg::bison::term::terminal (Linux/MSYS2 forkpty() behavior).

#include "src/term/terminal.hpp"

#include <gtest/gtest.h>

using bdg::bison::term::terminal;

TEST(Terminal, SpawnsHandlesAndExitsCleanly) {
  terminal t{"/bin/true"};
  EXPECT_GE(t.read_handle(), 0);
  EXPECT_GE(t.write_handle(), 0);
  EXPECT_EQ(t.wait(), 0);
}

TEST(Terminal, WaitReflectsNonZeroExitStatus) {
  terminal t{"/bin/false"};
  EXPECT_EQ(t.wait(), 1);
}

TEST(Terminal, WaitIsIdempotentAfterReap) {
  terminal t{"/bin/true"};
  EXPECT_EQ(t.wait(), 0);
  // Already reaped; a second wait() must not block or double-reap.
  EXPECT_EQ(t.wait(), -1);
}

TEST(Terminal, ExecFailureExitsWith127) {
  terminal t{"/no/such/executable"};
  EXPECT_EQ(t.wait(), 127);
}

TEST(Terminal, ReadAndWriteHandleShareOnePtyMaster) {
  // On POSIX, forkpty() yields a single bidirectional master fd — unlike
  // ConPTY's two unidirectional pipes on Windows (see terminal_win.cpp).
  terminal t{"/bin/true"};
  EXPECT_EQ(t.read_handle(), t.write_handle());
  t.wait();
}

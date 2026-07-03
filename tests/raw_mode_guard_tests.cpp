// MIT License © 2025 Binary Dice Games
// Tests for bdg::bison::pty::raw_mode_guard (Linux termios behavior).

#include "src/pty/raw_mode_guard.hpp"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <pty.h>
#include <termios.h>
#include <unistd.h>

using bdg::bison::pty::raw_mode_guard;

namespace {

/** @brief RAII pty pair so tests have a real tty fd without forking a process. */
struct pty_pair {
  int master{-1};
  int slave{-1};

  pty_pair() { EXPECT_EQ(openpty(&master, &slave, nullptr, nullptr, nullptr), 0); }
  ~pty_pair() {
    if (master >= 0)
      close(master);
    if (slave >= 0)
      close(slave);
  }
};

} // namespace

TEST(RawModeGuard, PutsTerminalIntoRawModeForItsLifetime) {
  pty_pair pty;

  struct termios before {};
  ASSERT_EQ(tcgetattr(pty.slave, &before), 0);
  ASSERT_TRUE((before.c_lflag & ICANON) != 0) << "openpty()'s default slave termios should start in cooked mode";

  {
    raw_mode_guard guard{pty.slave};
    struct termios during {};
    ASSERT_EQ(tcgetattr(pty.slave, &during), 0);
    EXPECT_EQ(during.c_lflag & ICANON, 0u);
    EXPECT_EQ(during.c_lflag & ECHO, 0u);
  }

  struct termios after {};
  ASSERT_EQ(tcgetattr(pty.slave, &after), 0);
  EXPECT_NE(after.c_lflag & ICANON, 0u) << "destructor should restore the original cooked-mode termios";
}

TEST(RawModeGuard, NonTerminalFdIsANoOp) {
  int fds[2]{};
  ASSERT_EQ(pipe(fds), 0);

  // Must not throw or crash on a fd that isn't a terminal at all.
  { raw_mode_guard guard{fds[0]}; }

  close(fds[0]);
  close(fds[1]);
}

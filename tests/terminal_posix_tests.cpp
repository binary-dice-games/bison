// MIT License © 2025 Binary Dice Games
// Tests for bdg::bison::term::terminal (Linux/MSYS2 forkpty() behavior).

#include "src/term/terminal.hpp"

#include <gtest/gtest.h>

#include <unistd.h>

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

// terminal redirects fd 1/2 through a byte-level CRLF translator for its
// lifetime (see terminal.hpp's class doc comment) -- these tests hijack fd 1
// to a test-owned pipe *before* constructing terminal, so the object's own
// dup() of "the real stdout" captures that pipe, letting the tests observe
// what the redirect's pump thread actually writes.

TEST(Terminal, RawStdioWriteDuringRedirectIsCrlfTranslatedImmediately) {
  int pipe_fds[2];
  ASSERT_EQ(pipe(pipe_fds), 0);
  const int saved_stdout = dup(STDOUT_FILENO);
  ASSERT_GE(saved_stdout, 0);
  ASSERT_GE(dup2(pipe_fds[1], STDOUT_FILENO), 0);
  close(pipe_fds[1]);

  {
    terminal t{"/bin/true"};

    // Raw write(), not fprintf/print() -- bypasses stdio buffering
    // entirely, simulating a third-party library (e.g. civetweb's
    // fprintf-based debug tracing) that doesn't know about terminal at all.
    ASSERT_EQ(write(STDOUT_FILENO, "hello\n", 6), 6);

    // Read while the terminal (and its redirect) is still alive: this is
    // the regression guard for "immediate", not "on flush/destruction".
    char buf[64] = {};
    const ssize_t n = read(pipe_fds[0], buf, sizeof(buf) - 1);
    ASSERT_GT(n, 0);
    EXPECT_EQ(std::string(buf, static_cast<size_t>(n)), "hello\r\n");

    t.wait();
  } // destructor restores fd 1 to what it was at construction (our pipe)

  ASSERT_GE(dup2(saved_stdout, STDOUT_FILENO), 0);
  close(saved_stdout);
  close(pipe_fds[0]);
}

TEST(Terminal, PartialWriteWithNoTrailingNewlinePassesThroughImmediately) {
  int pipe_fds[2];
  ASSERT_EQ(pipe(pipe_fds), 0);
  const int saved_stdout = dup(STDOUT_FILENO);
  ASSERT_GE(saved_stdout, 0);
  ASSERT_GE(dup2(pipe_fds[1], STDOUT_FILENO), 0);
  close(pipe_fds[1]);

  {
    terminal t{"/bin/true"};

    ASSERT_EQ(write(STDOUT_FILENO, "partial", 7), 7);

    char buf[64] = {};
    const ssize_t n = read(pipe_fds[0], buf, sizeof(buf) - 1);
    ASSERT_GT(n, 0);
    EXPECT_EQ(std::string(buf, static_cast<size_t>(n)), "partial");

    t.wait();
  }

  ASSERT_GE(dup2(saved_stdout, STDOUT_FILENO), 0);
  close(saved_stdout);
  close(pipe_fds[0]);
}

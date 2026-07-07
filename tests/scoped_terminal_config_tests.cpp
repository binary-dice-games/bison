// MIT License © 2025 Binary Dice Games
// Tests for bdg::bison::term::scoped_terminal_config.

#include "src/term/scoped_terminal_config.hpp"

#include <gtest/gtest.h>

#include <iostream>
#include <sstream>
#include <string>

#if !defined(_WIN32)
#include <pty.h>
#include <termios.h>
#include <unistd.h>
#endif

using bdg::bison::term::scoped_terminal_config;

#if !defined(_WIN32)

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

TEST(ScopedTerminalConfig, PutsTerminalIntoRawModeForItsLifetime) {
  pty_pair pty;

  struct termios before {};
  ASSERT_EQ(tcgetattr(pty.slave, &before), 0);
  ASSERT_TRUE((before.c_lflag & ICANON) != 0) << "openpty()'s default slave termios should start in cooked mode";

  {
    scoped_terminal_config config{{.read_fd = pty.slave, .write_fd = pty.slave}};
    struct termios during {};
    ASSERT_EQ(tcgetattr(pty.slave, &during), 0);
    EXPECT_EQ(during.c_lflag & ICANON, 0u);
    EXPECT_EQ(during.c_lflag & ECHO, 0u);
  }

  struct termios after {};
  ASSERT_EQ(tcgetattr(pty.slave, &after), 0);
  EXPECT_NE(after.c_lflag & ICANON, 0u) << "destructor should restore the original cooked-mode termios";
}

TEST(ScopedTerminalConfig, NonTerminalFdIsANoOpForRawMode) {
  int fds[2]{};
  ASSERT_EQ(pipe(fds), 0);

  // Must not throw or crash on a fd that isn't a terminal at all.
  { scoped_terminal_config config{{.read_fd = fds[0], .write_fd = fds[1]}}; }

  close(fds[0]);
  close(fds[1]);
}

#endif // !defined(_WIN32)

TEST(ScopedTerminalConfig, NoSinkRewritesNewlinesToRealDestination) {
  std::ostringstream captured;
  std::streambuf* original = std::cout.rdbuf(captured.rdbuf());

  {
    scoped_terminal_config config{{.read_fd = -1, .write_fd = -1}};
    std::cout << "line one\nline two\n";
  }

  std::cout.rdbuf(original);
  EXPECT_EQ(captured.str(), "line one\r\nline two\r\n");
}

TEST(ScopedTerminalConfig, RestoresOriginalStreambufOnDestruction) {
  std::ostringstream captured;
  std::streambuf* original = std::cout.rdbuf(captured.rdbuf());

  {
    scoped_terminal_config config{{.read_fd = -1, .write_fd = -1}};
    std::cout << "inside\n";
  }
  std::cout << "outside\n";

  std::cout.rdbuf(original);
  EXPECT_EQ(captured.str(), "inside\r\noutside\n");
}

TEST(ScopedTerminalConfig, SinkForwardsTranslatedBytesInsteadOfWritingTheRealFd) {
  std::ostringstream real_destination;
  std::streambuf* original = std::cout.rdbuf(real_destination.rdbuf());

  std::string sunk;
  {
    scoped_terminal_config config{
        {.read_fd = -1, .write_fd = -1, .sink = [&](std::string_view s) { sunk.append(s); }}};
    std::cout << "hello\nworld\n";
  }

  std::cout.rdbuf(original);
  EXPECT_EQ(sunk, "hello\r\nworld\r\n");
  EXPECT_TRUE(real_destination.str().empty()) << "sink mode must not also write the real destination";
}

TEST(ScopedTerminalConfig, AppliesToStderrToo) {
  std::ostringstream captured;
  std::streambuf* original = std::cerr.rdbuf(captured.rdbuf());

  {
    scoped_terminal_config config{{.read_fd = -1, .write_fd = -1}};
    std::cerr << "error: bad thing\n";
  }

  std::cerr.rdbuf(original);
  EXPECT_EQ(captured.str(), "error: bad thing\r\n");
}

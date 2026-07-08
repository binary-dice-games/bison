// MIT License © 2025 Binary Dice Games
// Tests for bdg::bison::term::scoped_terminal_config (Linux/MSYS2 behavior;
// native-Windows console-mode/pipe path is untested here, same as
// terminal_posix_tests.cpp).

#include "src/term/scoped_terminal_config.hpp"

#include <gtest/gtest.h>

#include <pty.h>
#include <termios.h>
#include <unistd.h>
#include <atomic>
#include <string>
#include <string_view>
#include <vector>

using bdg::bison::term::scoped_terminal_config;

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

/** @brief Reads up to @p max_bytes from @p fd, blocking until at least one byte arrives. */
std::string read_available(int fd, size_t max_bytes = 4096) {
  std::vector<char> buf(max_bytes);
  const ssize_t n = read(fd, buf.data(), buf.size());
  return n > 0 ? std::string(buf.data(), static_cast<size_t>(n)) : std::string{};
}

} // namespace

TEST(ScopedTerminalConfig, PutsTerminalIntoRawModeForItsLifetime) {
  pty_pair pty;
  int write_fds[2]{};
  ASSERT_EQ(pipe(write_fds), 0);

  struct termios before {};
  ASSERT_EQ(tcgetattr(pty.slave, &before), 0);
  ASSERT_TRUE((before.c_lflag & ICANON) != 0) << "openpty()'s default slave termios should start in cooked mode";

  {
    // read_fd and write_fd must be distinct fds — read_fd gets redirected
    // to an internal pipe, so raw-mode/termios checks below go through
    // upstream_read_fd(), the dup'd-aside copy of the real pty slave.
    scoped_terminal_config config{{.read_fd = pty.slave, .write_fd = write_fds[1]}};
    struct termios during {};
    ASSERT_EQ(tcgetattr(config.upstream_read_fd(), &during), 0);
    EXPECT_EQ(during.c_lflag & ICANON, 0u);
    EXPECT_EQ(during.c_lflag & ECHO, 0u);
  }

  struct termios after {};
  ASSERT_EQ(tcgetattr(pty.slave, &after), 0);
  EXPECT_NE(after.c_lflag & ICANON, 0u) << "destructor should restore the original cooked-mode termios";

  close(write_fds[0]);
  close(write_fds[1]);
}

TEST(ScopedTerminalConfig, NonTerminalFdIsANoOpForRawMode) {
  int fds[2]{};
  ASSERT_EQ(pipe(fds), 0);

  // Must not throw or crash on a fd that isn't a terminal at all.
  { scoped_terminal_config config{{.read_fd = fds[0], .write_fd = fds[1]}}; }

  close(fds[0]);
  close(fds[1]);
}

TEST(ScopedTerminalConfig, UpstreamFdsAreDistinctFromRedirectedOnes) {
  int read_fds[2]{};
  int write_fds[2]{};
  ASSERT_EQ(pipe(read_fds), 0);
  ASSERT_EQ(pipe(write_fds), 0);
  const int read_fd = read_fds[0];
  const int write_fd = write_fds[1];

  scoped_terminal_config config{{.read_fd = read_fd, .write_fd = write_fd}};
  EXPECT_GE(config.upstream_read_fd(), 0);
  EXPECT_GE(config.upstream_write_fd(), 0);
  EXPECT_NE(config.upstream_read_fd(), read_fd);
  EXPECT_NE(config.upstream_write_fd(), write_fd);

  close(read_fds[1]);
  close(write_fds[0]);
}

TEST(ScopedTerminalConfig, NoOutputChannelWritesTranslatedBytesToUpstreamFd) {
  int write_fds[2]{};
  ASSERT_EQ(pipe(write_fds), 0);
  const int write_fd = write_fds[1];

  scoped_terminal_config config{{.read_fd = -1 /* unused for this test */, .write_fd = write_fd}};
  // read_fd is intentionally invalid above (raw-mode/input redirection just
  // no-ops on it); only write_fd redirection is under test here.
  // upstream_write_fd() is a dup of write_fd itself (the pump writes
  // through it), so the translated bytes are observed by reading the
  // *other* end of the pipe this test created, write_fds[0].

  const std::string_view line = "hello\nworld\n";
  ASSERT_EQ(write(write_fd, line.data(), line.size()), static_cast<ssize_t>(line.size()));

  EXPECT_EQ(read_available(write_fds[0]), "hello\r\nworld\r\n");
}

TEST(ScopedTerminalConfig, OutputChannelReceivesTranslatedBytesInsteadOfUpstreamFd) {
  int write_fds[2]{};
  ASSERT_EQ(pipe(write_fds), 0);
  const int write_fd = write_fds[1];

  scoped_terminal_config config{{.read_fd = -1, .write_fd = write_fd}};

  std::string channeled;
  std::atomic<bool> received{false};
  config.set_output_channel([&](std::string_view s) {
    channeled.append(s);
    received.store(true);
  });

  const std::string_view line = "hello\nworld\n";
  ASSERT_EQ(write(write_fd, line.data(), line.size()), static_cast<ssize_t>(line.size()));

  for (int i = 0; i < 100 && !received.load(); ++i)
    usleep(10 * 1000);

  EXPECT_EQ(channeled, "hello\r\nworld\r\n");
}

TEST(ScopedTerminalConfig, OnPassthroughDeliversWholeLineOnNewline) {
  int read_fds[2]{};
  int write_fds[2]{};
  ASSERT_EQ(pipe(read_fds), 0);
  ASSERT_EQ(pipe(write_fds), 0);
  const int read_fd = read_fds[0];

  scoped_terminal_config config{{.read_fd = read_fd, .write_fd = write_fds[1]}};
  // Not yet delivered: on_passthrough buffers until a full line, matching
  // what canonical (cooked) mode would have buffered for a reader anyway.
  config.on_passthrough("hel");
  config.on_passthrough("lo\n");

  EXPECT_EQ(read_available(read_fd), "hello\n");
  close(read_fds[1]);
  // write_fds[0] deliberately left open -- stop_output_pump() (run by
  // config's destructor) restores real fd 2 (stderr) to point at
  // write_fds[1]'s upstream target; closing write_fds[0] here would leave
  // stderr writing into a pipe with no reader for the rest of the process.
}

TEST(ScopedTerminalConfig, OnPassthroughTranslatesBareCrToNewline) {
  int read_fds[2]{};
  int write_fds[2]{};
  ASSERT_EQ(pipe(read_fds), 0);
  ASSERT_EQ(pipe(write_fds), 0);
  const int read_fd = read_fds[0];

  scoped_terminal_config config{{.read_fd = read_fd, .write_fd = write_fds[1]}};
  // Real terminals send a bare '\r' for Enter (raw mode disables the usual
  // kernel/console CR->LF translation); on_passthrough must still terminate
  // the line for readers like std::getline() that only look for '\n'.
  config.on_passthrough("hello\r");

  EXPECT_EQ(read_available(read_fd), "hello\n");
  close(read_fds[1]); // write_fds[0] deliberately left open; see the previous test's comment.
}

TEST(ScopedTerminalConfig, OnPassthroughEchoesTypedBytesThroughOutputChannel) {
  int read_fds[2]{};
  int write_fds[2]{};
  ASSERT_EQ(pipe(read_fds), 0);
  ASSERT_EQ(pipe(write_fds), 0);
  const int read_fd = read_fds[0];
  const int write_fd = write_fds[1];

  scoped_terminal_config config{{.read_fd = read_fd, .write_fd = write_fd}};
  std::string echoed;
  std::atomic<bool> received{false};
  config.set_output_channel([&](std::string_view s) {
    echoed.append(s);
    received.store(true);
  });

  // Raw mode also disables the terminal's own local echo -- on_passthrough
  // must echo back what's "typed" (CRLF-translated, like any other output)
  // so the operator sees it, even before the line is complete.
  config.on_passthrough("hi");

  for (int i = 0; i < 100 && !received.load(); ++i)
    usleep(10 * 1000);

  EXPECT_EQ(echoed, "hi");
  close(read_fds[1]); // write_fds[0] deliberately left open; see the earlier test's comment.
}

TEST(ScopedTerminalConfig, OnPassthroughBackspaceErasesLastBufferedChar) {
  int read_fds[2]{};
  int write_fds[2]{};
  ASSERT_EQ(pipe(read_fds), 0);
  ASSERT_EQ(pipe(write_fds), 0);
  const int read_fd = read_fds[0];
  const int write_fd = write_fds[1];

  scoped_terminal_config config{{.read_fd = read_fd, .write_fd = write_fd}};
  std::string echoed;
  std::atomic<int> chunks_received{0};
  config.set_output_channel([&](std::string_view s) {
    echoed.append(s);
    chunks_received.fetch_add(1);
  });

  config.on_passthrough("helz");
  config.on_passthrough("\x7f"); // DEL: erase the mistyped 'z'
  config.on_passthrough("lo\n");

  for (int i = 0; i < 100 && chunks_received.load() < 3; ++i)
    usleep(10 * 1000);

  EXPECT_EQ(read_available(read_fd), "hello\n") << "the erased 'z' must not reach read_fd";
  // The echoed '\n' goes through the same output pipeline as any other
  // write to write_fd, so it picks up the usual '\n' -> "\r\n" translation.
  EXPECT_EQ(echoed, "helz\b \blo\r\n") << "backspace echoes as move-back, erase, move-back";
  close(read_fds[1]); // write_fds[0] deliberately left open; see the earlier test's comment.
}

TEST(ScopedTerminalConfig, OnPassthroughEmptyChunkClosesReadFd) {
  int read_fds[2]{};
  ASSERT_EQ(pipe(read_fds), 0);
  const int read_fd = read_fds[0];

  scoped_terminal_config config{{.read_fd = read_fd, .write_fd = -1}};
  config.on_passthrough("");

  char buf[1];
  EXPECT_EQ(read(read_fd, buf, sizeof(buf)), 0) << "empty chunk should surface as EOF on read_fd";
  close(read_fds[1]);
}

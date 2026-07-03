// MIT License © 2025 Binary Dice Games
// Tests for bdg::bison::pty::to_crlf/write_raw (src/pty/pty_write.hpp).

#include "src/pty/pty_write.hpp"

#include <gtest/gtest.h>

#include <string>
#include <thread>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

using bdg::bison::pty::to_crlf;
using bdg::bison::pty::write_raw;

namespace {

bool make_pipe(int fds[2]) {
#if defined(_WIN32)
  return _pipe(fds, 65536, _O_BINARY) == 0;
#else
  return pipe(fds) == 0;
#endif
}

size_t read_fd(int fd, char* buf, size_t len) {
#if defined(_WIN32)
  return _read(fd, buf, static_cast<unsigned int>(len));
#else
  return read(fd, buf, len);
#endif
}

void close_fd(int fd) {
#if defined(_WIN32)
  _close(fd);
#else
  close(fd);
#endif
}

} // namespace

TEST(PtyWrite, ToCrlfInsertsCrBeforeEachNewline) {
  EXPECT_EQ(to_crlf("line one\nline two\n"), "line one\r\nline two\r\n");
}

TEST(PtyWrite, ToCrlfLeavesTextWithoutNewlinesUnchanged) {
  EXPECT_EQ(to_crlf("no newline here"), "no newline here");
}

TEST(PtyWrite, ToCrlfOnlyTouchesBytesImmediatelyBeforeANewline) {
  // to_crlf() inserts a \r right before every \n and leaves every other
  // byte (including a \r that isn't already followed by \n) untouched.
  EXPECT_EQ(to_crlf("a\rb\n"), "a\rb\r\n");
}

TEST(PtyWrite, WriteRawWritesAllBytesToTheFd) {
  int fds[2]{};
  ASSERT_TRUE(make_pipe(fds));

  write_raw(fds[1], "hello\r\nworld\r\n");
  close_fd(fds[1]);

  char buf[64]{};
  const size_t n = read_fd(fds[0], buf, sizeof(buf));
  close_fd(fds[0]);

  ASSERT_GT(n, 0);
  EXPECT_EQ(std::string(buf, static_cast<size_t>(n)), "hello\r\nworld\r\n");
}

TEST(PtyWrite, WriteRawHandlesLargeWritesThatMayRequireMultipleSyscalls) {
  int fds[2]{};
  ASSERT_TRUE(make_pipe(fds));

  const std::string big(200000, 'x'); // exceeds typical pipe buffer size

  std::thread writer([&] {
    write_raw(fds[1], big);
    close_fd(fds[1]);
  });

  std::string received;
  char buf[65536];
  size_t n;
  while ((n = read_fd(fds[0], buf, sizeof(buf))) > 0)
    received.append(buf, static_cast<size_t>(n));
  close_fd(fds[0]);
  writer.join();

  EXPECT_EQ(received, big);
}

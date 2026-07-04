// MIT License © 2025 Binary Dice Games
// Tests for bdg::bison::console::console_process (libuv uv_spawn behavior).

#include "src/console/console_process.hpp"

#include <gtest/gtest.h>

#include <unistd.h>

using bdg::bison::console::console_process;

TEST(ConsoleProcess, SpawnsPipedFdsAndExitsCleanly) {
  console_process p{"/bin/true"};
  EXPECT_GE(p.read_fd(), 0);
  EXPECT_GE(p.write_fd(), 0);
  EXPECT_EQ(p.wait(), 0);
}

TEST(ConsoleProcess, WaitReflectsNonZeroExitStatus) {
  console_process p{"/bin/false"};
  EXPECT_EQ(p.wait(), 1);
}

TEST(ConsoleProcess, WaitIsIdempotent) {
  console_process p{"/bin/true"};
  EXPECT_EQ(p.wait(), 0);
  EXPECT_EQ(p.wait(), 0);
}

TEST(ConsoleProcess, PipesDataToAndFromChild) {
  // "head -n 1" echoes back the first line it reads and exits on its own —
  // no need to close write_fd() to signal EOF (console_process owns that fd
  // and closes it itself in the destructor).
  console_process p{"head -n 1"};

  const std::string message = "hello console\n";
  ASSERT_EQ(write(p.write_fd(), message.data(), message.size()), static_cast<ssize_t>(message.size()));

  std::string received;
  char buf[64];
  while (received.size() < message.size()) {
    const ssize_t n = read(p.read_fd(), buf, sizeof(buf));
    ASSERT_GT(n, 0);
    received.append(buf, static_cast<size_t>(n));
  }
  EXPECT_EQ(received, message);
  EXPECT_EQ(p.wait(), 0);
}

TEST(ConsoleProcess, ExecFailureExitsNonZero) {
  console_process p{"/no/such/executable"};
  EXPECT_NE(p.wait(), 0);
}

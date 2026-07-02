// MIT License © 2025 Binary Dice Games
// Tests for bdg::bison::pty::pty_process (Linux forkpty() behavior).

#include "src/pty/pty_process.hpp"

#include <gtest/gtest.h>

using bdg::bison::pty::pty_process;

#if defined(__linux__)

TEST(PtyProcess, SpawnsMasterFdAndExitsCleanly) {
  pty_process p{"/bin/true"};
  EXPECT_GE(p.master_fd(), 0);
  EXPECT_EQ(p.wait(), 0);
}

TEST(PtyProcess, WaitReflectsNonZeroExitStatus) {
  pty_process p{"/bin/false"};
  EXPECT_EQ(p.wait(), 1);
}

TEST(PtyProcess, WaitIsIdempotentAfterReap) {
  pty_process p{"/bin/true"};
  EXPECT_EQ(p.wait(), 0);
  // Already reaped; a second wait() must not block or double-reap.
  EXPECT_EQ(p.wait(), -1);
}

TEST(PtyProcess, ExecFailureExitsWith127) {
  pty_process p{"/no/such/executable"};
  EXPECT_EQ(p.wait(), 127);
}

#else

TEST(PtyProcess, NotExercisedOnThisPlatform) {
  GTEST_SKIP() << "pty_process Linux behavior is only exercised on Linux builds";
}

#endif

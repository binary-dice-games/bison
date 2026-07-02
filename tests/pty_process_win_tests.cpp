// MIT License © 2025 Binary Dice Games
// Tests for bdg::bison::pty::pty_process on Windows (not implemented).

#include "src/bison/pty/pty_process.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

using bdg::bison::pty::pty_process;

#if defined(_WIN32)

TEST(PtyProcessWin, ConstructorThrowsNotImplemented) {
  EXPECT_THROW({ pty_process p{}; }, std::runtime_error);
}

#else

TEST(PtyProcessWin, NotExercisedOnThisPlatform) {
  GTEST_SKIP() << "pty_process Windows stub is only exercised on Windows builds";
}

#endif

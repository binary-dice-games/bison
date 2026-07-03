// MIT License © 2025 Binary Dice Games
// Tests for bdg::bison::pty::pty_process on Windows (not implemented).

#include "src/pty/pty_process.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

using bdg::bison::pty::pty_process;

TEST(PtyProcessWin, ConstructorThrowsNotImplemented) {
  EXPECT_THROW({ pty_process p{}; }, std::runtime_error);
}

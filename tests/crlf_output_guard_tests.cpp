// MIT License © 2025 Binary Dice Games
// Tests for bdg::bison::pty::crlf_output_guard.

#include "src/pty/crlf_output_guard.hpp"

#include <gtest/gtest.h>

#include <iostream>
#include <sstream>
#include <string>

using bdg::bison::pty::crlf_output_guard;

TEST(CrlfOutputGuard, DefaultConstructorRewritesNewlinesToRealDestination) {
  std::ostringstream captured;
  std::streambuf* original = std::cout.rdbuf(captured.rdbuf());

  {
    crlf_output_guard guard;
    std::cout << "line one\nline two\n";
  }

  std::cout.rdbuf(original);
  EXPECT_EQ(captured.str(), "line one\r\nline two\r\n");
}

TEST(CrlfOutputGuard, RestoresOriginalStreambufOnDestruction) {
  std::ostringstream captured;
  std::streambuf* original = std::cout.rdbuf(captured.rdbuf());

  {
    crlf_output_guard guard;
    std::cout << "inside\n";
  }
  std::cout << "outside\n";

  std::cout.rdbuf(original);
  EXPECT_EQ(captured.str(), "inside\r\noutside\n");
}

TEST(CrlfOutputGuard, SinkConstructorForwardsTranslatedBytesInsteadOfWritingTheRealFd) {
  std::ostringstream real_destination;
  std::streambuf* original = std::cout.rdbuf(real_destination.rdbuf());

  std::string sunk;
  {
    crlf_output_guard guard{[&](std::string_view s) { sunk.append(s); }};
    std::cout << "hello\nworld\n";
  }

  std::cout.rdbuf(original);
  EXPECT_EQ(sunk, "hello\r\nworld\r\n");
  EXPECT_TRUE(real_destination.str().empty()) << "sink mode must not also write the real destination";
}

TEST(CrlfOutputGuard, AppliesToStderrToo) {
  std::ostringstream captured;
  std::streambuf* original = std::cerr.rdbuf(captured.rdbuf());

  {
    crlf_output_guard guard;
    std::cerr << "error: bad thing\n";
  }

  std::cerr.rdbuf(original);
  EXPECT_EQ(captured.str(), "error: bad thing\r\n");
}

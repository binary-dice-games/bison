// MIT License © 2025 Binary Dice Games
// Tests for src/app/client/line_editor.hpp's line_edit_state.
//
// line_editor::read_line() itself needs a real tty to drive (raw mode,
// ReadConsoleInputW/read()), so it's exercised manually; the editing rules
// it's built on live in line_edit_state, which takes plain key_events and
// is fully testable headless.

#include "src/app/client/line_editor.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace bdg::bison::app;

namespace {

key_event chr(char c) {
  return {editor_key::char_input, c};
}

key_event key(editor_key k) {
  return {k, 0};
}

void type(line_edit_state& st, const std::string& s) {
  for (char c : s)
    ASSERT_EQ(st.apply(chr(c)), line_edit_state::outcome::editing);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Basic editing
// ─────────────────────────────────────────────────────────────────────────────

TEST(LineEditStateTest, TypingAppendsAndMovesCursor) {
  std::vector<std::string> history;
  line_edit_state st(history);
  type(st, "abc");
  EXPECT_EQ(st.buffer(), "abc");
  EXPECT_EQ(st.cursor(), 3u);
}

TEST(LineEditStateTest, BackspaceDeletesBeforeCursorAndStopsAtStart) {
  std::vector<std::string> history;
  line_edit_state st(history);
  type(st, "ab");
  st.apply(key(editor_key::backspace));
  EXPECT_EQ(st.buffer(), "a");
  EXPECT_EQ(st.cursor(), 1u);

  // Backspace at column 0 must not delete past the start of the line (this
  // is what protects the "> " prompt from being erased in a real terminal).
  st.apply(key(editor_key::backspace));
  st.apply(key(editor_key::backspace));
  EXPECT_EQ(st.buffer(), "");
  EXPECT_EQ(st.cursor(), 0u);
}

TEST(LineEditStateTest, ArrowsMoveCursorWithinBounds) {
  std::vector<std::string> history;
  line_edit_state st(history);
  type(st, "abc");

  st.apply(key(editor_key::arrow_left));
  st.apply(key(editor_key::arrow_left));
  EXPECT_EQ(st.cursor(), 1u);

  st.apply(key(editor_key::arrow_left));
  st.apply(key(editor_key::arrow_left)); // clamps at 0
  EXPECT_EQ(st.cursor(), 0u);

  for (int i = 0; i < 5; ++i)
    st.apply(key(editor_key::arrow_right)); // clamps at buffer size
  EXPECT_EQ(st.cursor(), 3u);
}

TEST(LineEditStateTest, InsertAndDeleteAtCursorPosition) {
  std::vector<std::string> history;
  line_edit_state st(history);
  type(st, "ac");
  st.apply(key(editor_key::arrow_left));
  st.apply(chr('b'));
  EXPECT_EQ(st.buffer(), "abc");
  EXPECT_EQ(st.cursor(), 2u);

  st.apply(key(editor_key::home));
  st.apply(key(editor_key::delete_forward));
  EXPECT_EQ(st.buffer(), "bc");
  EXPECT_EQ(st.cursor(), 0u);

  st.apply(key(editor_key::end));
  EXPECT_EQ(st.cursor(), st.buffer().size());
}

// ─────────────────────────────────────────────────────────────────────────────
// History
// ─────────────────────────────────────────────────────────────────────────────

TEST(LineEditStateTest, UpDownCyclesThroughHistoryNewestFirst) {
  std::vector<std::string> history{"first", "second", "third"};
  line_edit_state st(history);

  st.apply(key(editor_key::arrow_up));
  EXPECT_EQ(st.buffer(), "third");
  st.apply(key(editor_key::arrow_up));
  EXPECT_EQ(st.buffer(), "second");
  st.apply(key(editor_key::arrow_up));
  EXPECT_EQ(st.buffer(), "first");

  // Clamped at the oldest entry.
  st.apply(key(editor_key::arrow_up));
  EXPECT_EQ(st.buffer(), "first");

  st.apply(key(editor_key::arrow_down));
  EXPECT_EQ(st.buffer(), "second");
  st.apply(key(editor_key::arrow_down));
  EXPECT_EQ(st.buffer(), "third");
}

TEST(LineEditStateTest, DownPastNewestRestoresInProgressDraft) {
  std::vector<std::string> history{"first"};
  line_edit_state st(history);
  type(st, "draft");

  st.apply(key(editor_key::arrow_up));
  EXPECT_EQ(st.buffer(), "first");

  st.apply(key(editor_key::arrow_down));
  EXPECT_EQ(st.buffer(), "draft");
  EXPECT_EQ(st.cursor(), st.buffer().size());
}

TEST(LineEditStateTest, TypingWhileBrowsingHistoryExitsBrowseMode) {
  std::vector<std::string> history{"first"};
  line_edit_state st(history);
  st.apply(key(editor_key::arrow_up));
  ASSERT_EQ(st.buffer(), "first");

  st.apply(chr('!'));
  EXPECT_EQ(st.buffer(), "first!");

  // No longer browsing: Down should not snap back to a history entry, and
  // since there was no draft (browsing started from an empty line), Up here
  // starts a fresh browse from the current (edited) buffer.
  st.apply(key(editor_key::arrow_down));
  EXPECT_EQ(st.buffer(), "first!");
}

TEST(LineEditStateTest, EmptyHistoryArrowsAreNoOps) {
  std::vector<std::string> history;
  line_edit_state st(history);
  type(st, "abc");
  st.apply(key(editor_key::arrow_up));
  st.apply(key(editor_key::arrow_down));
  EXPECT_EQ(st.buffer(), "abc");
}

// ─────────────────────────────────────────────────────────────────────────────
// Enter / Ctrl+C / Ctrl+D
// ─────────────────────────────────────────────────────────────────────────────

TEST(LineEditStateTest, EnterSubmits) {
  std::vector<std::string> history;
  line_edit_state st(history);
  type(st, "hello");
  EXPECT_EQ(st.apply(key(editor_key::enter)), line_edit_state::outcome::submitted);
  EXPECT_EQ(st.buffer(), "hello");
}

TEST(LineEditStateTest, InterruptCancels) {
  std::vector<std::string> history;
  line_edit_state st(history);
  type(st, "hello");
  EXPECT_EQ(st.apply(key(editor_key::interrupt)), line_edit_state::outcome::cancelled);
}

TEST(LineEditStateTest, ResetClearsBufferAndCursor) {
  std::vector<std::string> history{"a"};
  line_edit_state st(history);
  type(st, "hello");
  st.apply(key(editor_key::arrow_up));
  st.reset();
  EXPECT_EQ(st.buffer(), "");
  EXPECT_EQ(st.cursor(), 0u);
}

TEST(LineEditStateTest, EofOnEmptyBufferSignalsEof) {
  std::vector<std::string> history;
  line_edit_state st(history);
  EXPECT_EQ(st.apply(key(editor_key::eof)), line_edit_state::outcome::eof);
}

TEST(LineEditStateTest, EofOnNonEmptyBufferDeletesForwardInstead) {
  std::vector<std::string> history;
  line_edit_state st(history);
  type(st, "abc");
  st.apply(key(editor_key::home));
  EXPECT_EQ(st.apply(key(editor_key::eof)), line_edit_state::outcome::editing);
  EXPECT_EQ(st.buffer(), "bc");
}

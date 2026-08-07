// MIT License © 2025 Binary Dice Games
/**
 * @file line_editor.hpp
 * @brief Interactive terminal line editor with arrow-key command history.
 */
#pragma once

#include "src/bison/bison_sync.hpp"

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bdg::bison::app {

/** @brief Logical key produced by decoding a raw keystroke. */
enum class editor_key {
  char_input, ///< Printable character; see `key_event::ch`.
  backspace,
  delete_forward,
  arrow_left,
  arrow_right,
  arrow_up,
  arrow_down,
  home,
  end,
  enter,
  eof, ///< Ctrl+D on an empty line, or the input stream actually closing.
  interrupt, ///< Ctrl+C.
  ignored, ///< Recognized but not editing-relevant (e.g. Tab); no-op.
};

/** @brief One decoded keystroke. `ch` is only meaningful for `char_input`. */
struct key_event {
  editor_key key;
  char ch{};
};

/**
 * @brief Pure, terminal-agnostic line-editing state: the in-progress input
 *        buffer, cursor position, and Up/Down navigation through a shared
 *        command history.
 *
 * Holds no terminal I/O of its own, so the editing rules (cursor movement,
 * history recall, when Ctrl+D means EOF vs. delete-forward) are unit
 * testable by feeding synthetic `key_event`s, without a real tty.
 */
class line_edit_state {
 public:
  enum class outcome { editing, submitted, cancelled, eof };

  /** @param history Shared history buffer; outlives this object. Not owned. */
  explicit line_edit_state(std::vector<std::string>& history) : history_(history) {}

  /** @brief Applies one key event, mutating the buffer/cursor as needed. */
  outcome apply(const key_event& ev);

  /** @brief Discards the in-progress buffer and any active history browse. */
  void reset();

  const std::string& buffer() const { return buffer_; }
  size_t cursor() const { return cursor_; }

 private:
  void navigate_history(int delta);

  std::vector<std::string>& history_;
  std::string buffer_;
  size_t cursor_{0};
  std::optional<size_t> history_index_; // unset == not currently browsing
  std::string draft_; // buffer_ saved when history browsing began
};

/**
 * @brief Reads REPL input a line at a time, matching the interactive
 *        editing behavior of a typical shell or Python REPL: Left/Right
 *        move the cursor within the line, Backspace/Delete edit at the
 *        cursor (never past the start of the line, so the prompt itself
 *        can't be erased), and Up/Down recall previously submitted lines.
 *
 * Two ways to feed it keystrokes:
 * - Direct tty ownership (the default): when stdin/stdout are an
 *   interactive terminal (`is_interactive()`), `read_line()` puts the
 *   terminal in raw mode itself and reads keystrokes directly. Otherwise it
 *   falls back to plain `std::getline(std::cin, ...)`, so piped/redirected
 *   input (scripting) is unaffected.
 * - External feed (`enable_external_feed()` / `feed()`): for a caller where
 *   something else already owns raw-mode input delivery and can only hand
 *   over already-unframed keystroke bytes from a different thread (e.g.
 *   `term::scoped_terminal_config` under `--transport=term`, where fd 0 is a
 *   redirected pipe rather than the real terminal, so `is_interactive()` is
 *   false even though the far end is an interactive terminal).
 */
class line_editor {
 public:
  line_editor();
  ~line_editor();

  line_editor(const line_editor&) = delete;
  line_editor& operator=(const line_editor&) = delete;

  /**
   * @brief Prints @p prompt, then reads and echoes one line of input.
   *
   * Non-blank submitted lines that differ from the most recent history
   * entry are appended to this editor's history for subsequent Up/Down
   * recall.
   *
   * @param prompt Text to print before reading (e.g. "> ").
   * @param out    Receives the submitted line, without a trailing newline.
   * @return `false` on EOF (Ctrl+D on an empty line, or the underlying
   *         stream closing); `true` otherwise.
   */
  bool read_line(std::string_view prompt, std::string& out);

  /** @brief `true` if stdin and stdout are both an interactive terminal. */
  static bool is_interactive();

  /**
   * @brief Switches into external-feed mode (see class doc comment): reads
   *        keystrokes from `feed()` instead of owning the terminal itself.
   *        Must be called before the first `read_line()`.
   */
  void enable_external_feed();

  /**
   * @brief Delivers one chunk of raw keystroke bytes in external-feed mode;
   *        a no-op otherwise. Thread-safe -- typically called from whatever
   *        thread is pumping the transport, not the thread blocked in
   *        `read_line()`.
   *
   * @param chunk Raw bytes; an empty chunk signals EOF.
   */
  void feed(std::string_view chunk);

 private:
  /** @brief Opaque platform-specific state (saved terminal mode, handles). */
  struct impl;
  using impl_ptr = std::unique_ptr<impl, void (*)(impl*)>;

  /** @brief Puts the terminal in raw mode; null if `is_interactive()` is false. */
  static impl_ptr create_impl();

  /** @brief Blocking read of the next decoded keystroke from the owned tty. Defined per platform. */
  static key_event read_key(impl& state);

  /**
   * @brief Decodes one keystroke from a raw byte stream: plain bytes plus
   *        ANSI/VT escape sequences (arrows, Home/End, Delete). Shared by
   *        every backend that consumes a byte-at-a-time stream (POSIX direct
   *        tty and external-feed mode on every platform, since pass-through
   *        bytes from a real remote terminal are the same ANSI byte stream
   *        regardless of the local platform).
   *
   * @param next_byte Callable that fetches the next byte via its out-param;
   *                   returns `false` on EOF.
   */
  static key_event decode_byte_stream(const std::function<bool(unsigned char&)>& next_byte);

  /** @brief Runs the redraw/key/apply loop against @p next_key until submit/EOF. */
  bool run_edit_loop(std::string_view prompt, std::string& out, const std::function<key_event()>& next_key);

  /** @brief Blocking pop of the next externally-fed byte. @return `false` on EOF. */
  bool pop_fed_byte(unsigned char& out);

  struct feed_queue_state {
    std::deque<char> bytes;
    bool eof{false};
  };

  impl_ptr impl_;
  bool external_mode_{false};
  bison::synchronized<feed_queue_state> feed_queue_;
  std::vector<std::string> history_;
};

} // namespace bdg::bison::app

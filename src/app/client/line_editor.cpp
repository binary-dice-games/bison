// MIT License © 2025 Binary Dice Games
/**
 * @file line_editor.cpp
 * @brief Platform-independent editing rules (`line_edit_state`) and the
 *        `line_editor::read_line()` render loop built on top of them.
 *        Raw-mode setup and keystroke decoding are per platform, in
 *        line_editor_posix.cpp / line_editor_win.cpp.
 */
#include "src/app/client/line_editor.hpp"

#include <cstdio>
#include <iostream>

namespace bdg::bison::app {

// ── line_edit_state ───────────────────────────────────────────────────────────

line_edit_state::outcome line_edit_state::apply(const key_event& ev) {
  switch (ev.key) {
    case editor_key::char_input:
      buffer_.insert(buffer_.begin() + static_cast<ptrdiff_t>(cursor_), ev.ch);
      ++cursor_;
      history_index_.reset();
      return outcome::editing;

    case editor_key::backspace:
      if (cursor_ > 0) {
        buffer_.erase(buffer_.begin() + static_cast<ptrdiff_t>(--cursor_));
        history_index_.reset();
      }
      return outcome::editing;

    case editor_key::delete_forward:
      if (cursor_ < buffer_.size()) {
        buffer_.erase(buffer_.begin() + static_cast<ptrdiff_t>(cursor_));
        history_index_.reset();
      }
      return outcome::editing;

    case editor_key::arrow_left:
      if (cursor_ > 0)
        --cursor_;
      return outcome::editing;

    case editor_key::arrow_right:
      if (cursor_ < buffer_.size())
        ++cursor_;
      return outcome::editing;

    case editor_key::home:
      cursor_ = 0;
      return outcome::editing;

    case editor_key::end:
      cursor_ = buffer_.size();
      return outcome::editing;

    case editor_key::arrow_up:
      navigate_history(-1);
      return outcome::editing;

    case editor_key::arrow_down:
      navigate_history(1);
      return outcome::editing;

    case editor_key::enter:
      return outcome::submitted;

    case editor_key::interrupt:
      return outcome::cancelled;

    case editor_key::eof:
      if (buffer_.empty())
        return outcome::eof;
      if (cursor_ < buffer_.size()) {
        buffer_.erase(buffer_.begin() + static_cast<ptrdiff_t>(cursor_));
        history_index_.reset();
      }
      return outcome::editing;

    case editor_key::ignored:
    default:
      return outcome::editing;
  }
}

void line_edit_state::reset() {
  buffer_.clear();
  cursor_ = 0;
  history_index_.reset();
  draft_.clear();
}

void line_edit_state::navigate_history(int delta) {
  if (history_.empty())
    return;

  if (!history_index_) {
    if (delta > 0)
      return; // already at the newest (no draft) line -- Down is a no-op
    draft_ = buffer_;
    history_index_ = history_.size() - 1;
  } else {
    const long next = static_cast<long>(*history_index_) + delta;
    if (next < 0)
      return; // clamp at the oldest entry
    if (static_cast<size_t>(next) >= history_.size()) {
      // Moved past the newest entry -- restore what was being typed.
      history_index_.reset();
      buffer_ = draft_;
      cursor_ = buffer_.size();
      return;
    }
    history_index_ = static_cast<size_t>(next);
  }

  buffer_ = history_[*history_index_];
  cursor_ = buffer_.size();
}

// ── line_editor ────────────────────────────────────────────────────────────────

namespace {

/**
 * @brief Redraws the current prompt + buffer on a single terminal line,
 *        using ANSI/VT sequences (clear-line, cursor-left) supported by
 *        POSIX terminals and, on Windows, `ENABLE_VIRTUAL_TERMINAL_PROCESSING`
 *        (enabled by line_editor_win.cpp's raw-mode setup).
 */
void redraw(std::string_view prompt, const line_edit_state& state) {
  std::string out;
  out += '\r';
  out += "\x1b[K";
  out.append(prompt);
  out += state.buffer();
  const size_t back = state.buffer().size() - state.cursor();
  if (back > 0) {
    out += "\x1b[";
    out += std::to_string(back);
    out += 'D';
  }
  std::fwrite(out.data(), 1, out.size(), stdout);
  std::fflush(stdout);
}

void write_raw(std::string_view s) {
  std::fwrite(s.data(), 1, s.size(), stdout);
  std::fflush(stdout);
}

} // namespace

line_editor::line_editor() : impl_(create_impl()) {}

line_editor::~line_editor() = default;

bool line_editor::read_line(std::string_view prompt, std::string& out) {
  if (!impl_) {
    std::cout << prompt << std::flush;
    return static_cast<bool>(std::getline(std::cin, out));
  }

  line_edit_state state(history_);
  redraw(prompt, state);

  for (;;) {
    const key_event ev = read_key(*impl_);
    switch (state.apply(ev)) {
      case line_edit_state::outcome::editing:
        redraw(prompt, state);
        break;

      case line_edit_state::outcome::submitted:
        write_raw("\r\n");
        out = state.buffer();
        if (!out.empty() && (history_.empty() || history_.back() != out))
          history_.push_back(out);
        return true;

      case line_edit_state::outcome::cancelled:
        write_raw("^C\r\n");
        state.reset();
        redraw(prompt, state);
        break;

      case line_edit_state::outcome::eof:
        write_raw("\r\n");
        return false;
    }
  }
}

} // namespace bdg::bison::app

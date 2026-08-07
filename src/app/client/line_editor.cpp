// MIT License © 2025 Binary Dice Games
/**
 * @file line_editor.cpp
 * @brief Platform-independent editing rules (`line_edit_state`), the shared
 *        ANSI byte-stream keystroke decoder, external-feed mode, and the
 *        `line_editor::read_line()` render loop built on top of them.
 *        Direct-tty raw-mode setup and keystroke decoding are per platform,
 *        in line_editor_posix.cpp / line_editor_win.cpp.
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

// ── line_editor: shared ANSI byte-stream decoder ────────────────────────────────

key_event line_editor::decode_byte_stream(const std::function<bool(unsigned char&)>& next_byte) {
  unsigned char c;
  if (!next_byte(c))
    return {editor_key::eof, 0};

  if (c == '\r' || c == '\n')
    return {editor_key::enter, 0};
  if (c == 0x7f || c == 0x08)
    return {editor_key::backspace, 0};
  if (c == 0x04)
    return {editor_key::eof, 0};
  if (c == 0x03)
    return {editor_key::interrupt, 0};

  if (c == 0x1b) {
    unsigned char introducer;
    if (!next_byte(introducer) || (introducer != '[' && introducer != 'O'))
      return {editor_key::ignored, 0};

    unsigned char code;
    if (!next_byte(code))
      return {editor_key::ignored, 0};

    switch (code) {
      case 'A':
        return {editor_key::arrow_up, 0};
      case 'B':
        return {editor_key::arrow_down, 0};
      case 'C':
        return {editor_key::arrow_right, 0};
      case 'D':
        return {editor_key::arrow_left, 0};
      case 'H':
        return {editor_key::home, 0};
      case 'F':
        return {editor_key::end, 0};
      case '1': // ESC [ 1 ~  == Home (some terminals)
      case '7':
      case '3': // ESC [ 3 ~  == Delete
      case '4': // ESC [ 4 ~  == End (some terminals)
      case '8': {
        unsigned char tilde;
        next_byte(tilde); // consume the trailing '~'
        if (code == '3')
          return {editor_key::delete_forward, 0};
        if (code == '1' || code == '7')
          return {editor_key::home, 0};
        return {editor_key::end, 0};
      }
      default:
        return {editor_key::ignored, 0};
    }
  }

  if (c < 0x20)
    return {editor_key::ignored, 0};

  return {editor_key::char_input, static_cast<char>(c)};
}

// ── line_editor: external-feed mode ─────────────────────────────────────────────

void line_editor::enable_external_feed() {
  external_mode_ = true;
}

void line_editor::feed(std::string_view chunk) {
  if (chunk.empty()) {
    feed_queue_.wlock()->eof = true;
  } else {
    auto lp = feed_queue_.wlock();
    lp->bytes.insert(lp->bytes.end(), chunk.begin(), chunk.end());
  }
  feed_queue_.notify_all();
}

bool line_editor::pop_fed_byte(unsigned char& out) {
  bool got = false;
  feed_queue_.wait([&](feed_queue_state& qs) {
    if (!qs.bytes.empty()) {
      out = static_cast<unsigned char>(qs.bytes.front());
      qs.bytes.pop_front();
      got = true;
      return true;
    }
    return qs.eof; // stop waiting once EOF is signaled and the queue drains
  });
  return got;
}

// ── line_editor: render loop ────────────────────────────────────────────────────

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

line_editor::line_editor() : impl_(nullptr, [](impl*) {}) {}

line_editor::~line_editor() = default;

bool line_editor::run_edit_loop(std::string_view prompt, std::string& out, const std::function<key_event()>& next_key) {
  line_edit_state state(history_);
  redraw(prompt, state);

  for (;;) {
    const key_event ev = next_key();
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

bool line_editor::read_line(std::string_view prompt, std::string& out) {
  if (external_mode_) {
    return run_edit_loop(prompt, out, [this] {
      return decode_byte_stream([this](unsigned char& b) { return pop_fed_byte(b); });
    });
  }

  if (!impl_)
    impl_ = create_impl();
  if (!impl_) {
    std::cout << prompt << std::flush;
    return static_cast<bool>(std::getline(std::cin, out));
  }

  return run_edit_loop(prompt, out, [this] { return read_key(*impl_); });
}

} // namespace bdg::bison::app

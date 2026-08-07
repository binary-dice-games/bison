// MIT License © 2025 Binary Dice Games
/**
 * @file line_editor_posix.cpp
 * @brief Linux/MSYS2 half of line_editor: termios raw mode plus decoding
 *        keystrokes (including ANSI escape sequences for arrow/Home/End/
 *        Delete) read one byte at a time from stdin.
 */
#include "src/app/client/line_editor.hpp"

#include <termios.h>
#include <unistd.h>

namespace bdg::bison::app {

struct line_editor::impl {
  struct termios saved_termios{};
};

bool line_editor::is_interactive() {
  return isatty(STDIN_FILENO) != 0 && isatty(STDOUT_FILENO) != 0;
}

line_editor::impl_ptr line_editor::create_impl() {
  if (!is_interactive())
    return impl_ptr(nullptr, [](impl*) {});

  auto* state = new impl();
  tcgetattr(STDIN_FILENO, &state->saved_termios);
  struct termios raw = state->saved_termios;
  cfmakeraw(&raw);
  tcsetattr(STDIN_FILENO, TCSANOW, &raw);

  return impl_ptr(state, [](impl* s) {
    tcsetattr(STDIN_FILENO, TCSANOW, &s->saved_termios);
    delete s;
  });
}

namespace {

/** @brief Reads exactly one byte from stdin. @return `false` on EOF/error. */
bool read_byte(unsigned char& out) {
  const ssize_t n = ::read(STDIN_FILENO, &out, 1);
  return n == 1;
}

/** @brief Decodes the byte(s) following an ESC that introduced a CSI/SS3 sequence. */
key_event decode_escape_sequence() {
  unsigned char introducer;
  if (!read_byte(introducer) || (introducer != '[' && introducer != 'O'))
    return {editor_key::ignored, 0};

  unsigned char code;
  if (!read_byte(code))
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
      read_byte(tilde); // consume the trailing '~'
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

} // namespace

key_event line_editor::read_key(impl& /*state*/) {
  unsigned char c;
  if (!read_byte(c))
    return {editor_key::eof, 0};

  if (c == '\r' || c == '\n')
    return {editor_key::enter, 0};
  if (c == 0x7f || c == 0x08)
    return {editor_key::backspace, 0};
  if (c == 0x04)
    return {editor_key::eof, 0};
  if (c == 0x03)
    return {editor_key::interrupt, 0};
  if (c == 0x1b)
    return decode_escape_sequence();
  if (c < 0x20)
    return {editor_key::ignored, 0};

  return {editor_key::char_input, static_cast<char>(c)};
}

} // namespace bdg::bison::app

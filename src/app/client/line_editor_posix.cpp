// MIT License © 2025 Binary Dice Games
/**
 * @file line_editor_posix.cpp
 * @brief Linux/MSYS2 half of line_editor: termios raw mode plus a blocking
 *        single-byte stdin reader fed into the shared ANSI byte-stream
 *        decoder (line_editor.cpp's decode_byte_stream()).
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
  // Disable canonical mode, echo, and signal generation so every keystroke
  // -- including Ctrl+C -- arrives as a plain byte for decode_byte_stream()
  // to interpret. Deliberately leave c_oflag (OPOST/ONLCR) untouched: unlike
  // cfmakeraw(), which also strips output processing, this keeps '\n' -> "\r\n"
  // translation working for ordinary stdout writes elsewhere in the CLI.
  raw.c_lflag &= ~(static_cast<tcflag_t>(ECHO | ICANON | ISIG | IEXTEN));
  raw.c_iflag &= ~(static_cast<tcflag_t>(IXON | ICRNL | BRKINT | INPCK | ISTRIP));
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSANOW, &raw);

  return impl_ptr(state, [](impl* s) {
    tcsetattr(STDIN_FILENO, TCSANOW, &s->saved_termios);
    delete s;
  });
}

key_event line_editor::read_key(impl& /*state*/) {
  return decode_byte_stream([](unsigned char& out) { return ::read(STDIN_FILENO, &out, 1) == 1; });
}

} // namespace bdg::bison::app

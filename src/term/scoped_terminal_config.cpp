// MIT License © 2025 Binary Dice Games
/**
 * @file scoped_terminal_config.cpp
 * @brief Platform-independent constructor/destructor. Everything else —
 *        `impl`'s definition and the raw-mode/pipe/pump machinery — is
 *        implemented per platform in scoped_terminal_config_posix.cpp /
 *        _win.cpp.
 */
#include "src/term/scoped_terminal_config.hpp"

namespace bdg::bison::term {

void scoped_terminal_config::process_passthrough_chunk(
    std::string_view chunk,
    bool& last_was_cr,
    std::string& line_buffer,
    std::string& to_echo,
    std::string& to_deliver) {
  for (char c : chunk) {
    if (last_was_cr && c == '\n') {
      last_was_cr = false;
      continue; // second half of a "\r\n" pair -- already handled the '\r'
    }
    last_was_cr = false;

    if (c == '\r') {
      c = '\n';
      last_was_cr = true;
    }

    if (c == '\n') {
      line_buffer.push_back('\n');
      to_deliver.append(line_buffer);
      line_buffer.clear();
      to_echo.push_back('\n');
      continue;
    }

    if (c == '\b' || c == '\x7f') { // backspace or delete
      if (!line_buffer.empty()) {
        line_buffer.pop_back();
        to_echo.append("\b \b"); // move back, erase, move back
      }
      continue;
    }

    line_buffer.push_back(c);
    to_echo.push_back(c);
  }
}

void scoped_terminal_config::set_raw_input_sink(std::function<void(std::string_view)> sink) {
  *raw_input_sink_.wlock() = std::move(sink);
}

scoped_terminal_config::scoped_terminal_config(params&& p) : params_(std::move(p)), impl_(create_state(params_)) {}

scoped_terminal_config::~scoped_terminal_config() {
  // stop_output_pump() must run first, while impl_ (the member it reads)
  // is still valid -- release_state() immediately moves impl_ out from
  // under it. Its own internal call to stop_output_pump() (via impl_,
  // now null) is then a harmless no-op.
  stop_output_pump();
  release_state(std::move(impl_));
}

} // namespace bdg::bison::term

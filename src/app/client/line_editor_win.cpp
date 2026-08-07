// MIT License © 2025 Binary Dice Games
/**
 * @file line_editor_win.cpp
 * @brief Native-Windows half of line_editor: console-mode raw input plus
 *        decoding `KEY_EVENT_RECORD`s from `ReadConsoleInputW`.
 */
#include "src/app/client/line_editor.hpp"

#include <io.h>
#include <windows.h>

namespace bdg::bison::app {

struct line_editor::impl {
  HANDLE stdin_handle{};
  HANDLE stdout_handle{};
  DWORD saved_input_mode{};
  DWORD saved_output_mode{};
};

bool line_editor::is_interactive() {
  return _isatty(_fileno(stdin)) != 0 && _isatty(_fileno(stdout)) != 0;
}

line_editor::impl_ptr line_editor::create_impl() {
  if (!is_interactive())
    return impl_ptr(nullptr, [](impl*) {});

  auto* state = new impl();
  state->stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
  state->stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);
  GetConsoleMode(state->stdin_handle, &state->saved_input_mode);
  GetConsoleMode(state->stdout_handle, &state->saved_output_mode);

  // Disable line buffering, local echo, and Ctrl+C's default handling, so
  // every keystroke -- including Ctrl+C -- arrives as a plain key event.
  const DWORD raw_input_mode =
      state->saved_input_mode & ~static_cast<DWORD>(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
  SetConsoleMode(state->stdin_handle, raw_input_mode);

  // The renderer (line_editor.cpp's redraw()) emits ANSI/VT cursor and
  // clear-line sequences; enable VT processing so the console interprets
  // them instead of printing them literally.
  SetConsoleMode(state->stdout_handle, state->saved_output_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

  return impl_ptr(state, [](impl* s) {
    SetConsoleMode(s->stdin_handle, s->saved_input_mode);
    SetConsoleMode(s->stdout_handle, s->saved_output_mode);
    delete s;
  });
}

key_event line_editor::read_key(impl& state) {
  for (;;) {
    INPUT_RECORD record;
    DWORD read_count = 0;
    if (!ReadConsoleInputW(state.stdin_handle, &record, 1, &read_count) || read_count == 0)
      return {editor_key::eof, 0};
    if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown)
      continue;

    const KEY_EVENT_RECORD& key = record.Event.KeyEvent;
    const bool ctrl_down = (key.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;

    if (ctrl_down && (key.wVirtualKeyCode == 'C'))
      return {editor_key::interrupt, 0};
    if (ctrl_down && (key.wVirtualKeyCode == 'D'))
      return {editor_key::eof, 0};

    switch (key.wVirtualKeyCode) {
      case VK_RETURN:
        return {editor_key::enter, 0};
      case VK_BACK:
        return {editor_key::backspace, 0};
      case VK_DELETE:
        return {editor_key::delete_forward, 0};
      case VK_LEFT:
        return {editor_key::arrow_left, 0};
      case VK_RIGHT:
        return {editor_key::arrow_right, 0};
      case VK_UP:
        return {editor_key::arrow_up, 0};
      case VK_DOWN:
        return {editor_key::arrow_down, 0};
      case VK_HOME:
        return {editor_key::home, 0};
      case VK_END:
        return {editor_key::end, 0};
      default:
        break;
    }

    const wchar_t ch = key.uChar.UnicodeChar;
    if (ch >= 0x20 && ch < 0x7f)
      return {editor_key::char_input, static_cast<char>(ch)};
  }
}

} // namespace bdg::bison::app

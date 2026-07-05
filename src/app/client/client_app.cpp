// MIT License © 2025 Binary Dice Games
/**
 * @file client_app.cpp
 * @brief Generic multi-transport client application scaffold implementation.
 */
#include "src/app/client/client_app.hpp"

#include "src/app/debugger.hpp"
#include "src/app/transport_flags.hpp"
#include "src/pty/crlf_output_guard.hpp"
#include "src/pty/raw_mode_guard.hpp"
#include "src/rmi/client/client.hpp"
#include "src/rmi/transport/named_pipe_transport.hpp"
#include "src/rmi/transport/socket_transport.hpp"
#include "src/rmi/transport/stdio_transport.hpp"
#include "src/rmi/transport/term_transport.hpp"

#include <gflags/gflags.h>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

DECLARE_string(host);
DECLARE_int32(port);
DECLARE_string(name);
DECLARE_int32(timeout);
DECLARE_bool(debugger);

namespace bdg::bison::app {

// ── Default hook implementations ──────────────────────────────────────────────

void client_app::on_connect_params(bison::dynamic& params) const {
  params["timeout_ms"_key] = int32_t{static_cast<int32_t>(timeout_.count())};
}

void client_app::on_error(const std::string& msg) const {
  std::cerr << "[client_app] error: " << msg << '\n';
}

// ── Console input (see read_console_line()'s doc comment for why) ─────────────

void client_app::feed_console_passthrough(std::string_view chunk) {
  if (chunk.empty()) {
    console_queue_.withWLock([&](auto& st) { st.closed = true; });
    console_queue_.notify_all();
    return;
  }

  console_queue_.withWLock([&](auto& st) {
    for (const char c : chunk) {
      if (c == 0x7f || c == 0x08) { // DEL / BS: erase one character
        if (!st.partial.empty()) {
          st.partial.pop_back();
          if (echo_fn_)
            echo_fn_("\b \b");
        }
        continue;
      }
      if (echo_fn_)
        echo_fn_(c == '\n' ? std::string_view{"\r\n"} : std::string_view{&c, 1});
      st.partial.push_back(c);
      if (c == '\n') {
        st.lines.push(st.partial.substr(0, st.partial.size() - 1));
        st.partial.clear();
      }
    }
  });
  console_queue_.notify_all();
}

bool client_app::read_console_line(std::string& line) {
  if (!console_via_passthrough_)
    return static_cast<bool>(std::getline(std::cin, line));

  bool got = false;
  console_queue_.wait([&](auto& st) {
    if (!st.lines.empty()) {
      line = std::move(st.lines.front());
      st.lines.pop();
      got = true;
      return true;
    }
    return st.closed;
  });
  return got;
}

// ── run_with_transport ────────────────────────────────────────────────────────

int client_app::run_with_transport(std::unique_ptr<rmi::transport::client_transport_iface> transport) {
  rmi::client c{std::move(transport)};

  bison::dynamic params;
  on_connect_params(params);
  c.connect(std::move(params));
  on_connected();

  const int result = on_session(c);
  c.disconnect();
  return result;
}

// ── run — flag parsing and transport selection ────────────────────────────────

int client_app::run(int argc, char** argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_debugger) {
    wait_for_debugger();
  }

  timeout_ = std::chrono::milliseconds{FLAGS_timeout};

  try {
    const transport_kind transport = selected_transport();

    switch (transport) {
      case transport_kind::pty: {
        // fd 0/1 are a pty slave left in cooked mode (see src/pty/DESIGN.md):
        // its ICANON/ECHO processing would stall BISON<...> frames (no \n
        // terminator to release the kernel's line buffer) or echo them back
        // at the reader, so put it in raw mode for the RMI session and
        // restore it on the way out.
        pty::raw_mode_guard raw{0};
        // The transport's background reader owns fd 0 (non-blocking, scanning
        // for BISON<...> frames), so std::cin can't safely read it too — route
        // operator keystrokes through the passthrough callback instead; see
        // read_console_line().
        console_via_passthrough_ = true;
        auto stdio_transport = std::make_unique<rmi::transport::stdio_client_transport>(
            0, 1, [this](std::string_view chunk) { feed_console_passthrough(chunk); });
        // Raw pointer captured by value into echo_fn_ stays valid for the
        // rest of the process's lifetime: `c` (inside run_with_transport)
        // owns this transport until it returns, and nothing else in
        // --transport=pty mode runs after that. See echo_fn_'s doc comment
        // for what it's used for.
        auto* raw_transport = stdio_transport.get();
        echo_fn_ = [raw_transport](std::string_view s) { raw_transport->send(s); };
        // Raw mode also strips \r from this process's own std::cout/std::cerr
        // output (turning OPOST off is global to the fd, not scoped to frame
        // writes) — compensate so ordinary REPL output still displays
        // starting at the left margin instead of stairstepping. Routed
        // through the transport's own writer (send), not written to fd 1
        // directly: read_console_line()'s local echo *also* writes there via
        // send, and two independent, unsynchronized writers racing on the
        // same fd is exactly the kind of bug this codebase has been chasing
        // throughout --transport=pty support. See crlf_output_guard's doc comment.
        pty::crlf_output_guard output_guard{[this](std::string_view s) { echo_fn_(s); }};
        return run_with_transport(std::move(stdio_transport));
      }
      case transport_kind::pipe:
        return run_with_transport(std::make_unique<rmi::transport::named_pipe_client_transport>(FLAGS_name));
      case transport_kind::tcp:
        return run_with_transport(
            std::make_unique<rmi::transport::socket_client_transport>(FLAGS_host, static_cast<uint16_t>(FLAGS_port)));
      case transport_kind::console: {
        // No subprocess spawning here — the server is the one that spawns
        // this process (see server_app's --transport=console/--cmd); this
        // side just wraps its own inherited fd 0/1 in the BISON<...>
        // framing, same as --transport=pty's client case, but with no
        // raw_mode_guard/crlf_output_guard: there's no terminal here (fd 0/1
        // are piped, not a tty), so there's no termios/CRLF fallout to fix.
        console_via_passthrough_ = true;
        auto stdio_transport = std::make_unique<rmi::transport::stdio_client_transport>(
            0, 1, [this](std::string_view chunk) { feed_console_passthrough(chunk); });
        return run_with_transport(std::move(stdio_transport));
      }
      case transport_kind::term: {
        // Same raw-mode/CRLF rationale as --transport=pty above, but framed
        // as OSC-99 instead of BISON<...> (see term_transport.hpp).
        pty::raw_mode_guard raw{0};
        console_via_passthrough_ = true;
        auto term_transport = std::make_unique<rmi::transport::term_client_transport>(
            0, 1, [this](std::string_view chunk) { feed_console_passthrough(chunk); });
        auto* raw_transport = term_transport.get();
        echo_fn_ = [raw_transport](std::string_view s) { raw_transport->send(s); };
        pty::crlf_output_guard output_guard{[this](std::string_view s) { echo_fn_(s); }};
        return run_with_transport(std::move(term_transport));
      }
    }
    return 1;

  } catch (const std::exception& ex) {
    on_error(ex.what());
    return 1;
  } catch (...) {
    on_error("unexpected failure");
    return 1;
  }
}

} // namespace bdg::bison::app

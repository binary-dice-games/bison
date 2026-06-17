// MIT License © 2025 Binary Dice Games
/**
 * @file pty_client_transport.cpp
 * @brief PTY client transport implementation.
 *
 * Uses the process's own stdin/stdout (PTY slave or SSH channel) as a DCS
 * bison transport.  The client initiates the handshake: it emits HELLO first,
 * then waits for the server's HELLO response.
 */
#include "src/pty/pty_client_transport.hpp"

#if defined(__linux__)

#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bdg::bison::pty {

namespace {

// ── Framing constants ─────────────────────────────────────────────────────

constexpr char        kEsc          = '\x1b';
constexpr const char* kDcsStart     = "\x1bP";
constexpr const char* kDcsEnd       = "\x1b\\";
constexpr const char* kProtoVersion = "BISON_RMI/1";
constexpr const char* kTypeData     = "DATA";
constexpr const char* kTypeHello    = "HELLO";
constexpr const char* kTypeEnd      = "END";

// ── Reassembly record ─────────────────────────────────────────────────────

struct partial_message {
  uint32_t total = 0;
  std::vector<std::optional<bison::buffer>> parts;
  size_t collected = 0;
  std::chrono::steady_clock::time_point first_seen;
};

// ── Client transport state ────────────────────────────────────────────────

struct client_state {
  // ── I/O ───────────────────────────────────────────────────────────────────
  int read_fd  = STDIN_FILENO;
  int write_fd = STDOUT_FILENO;

  // ── Write lock ────────────────────────────────────────────────────────────
  std::mutex write_mtx;

  // ── Inbox (protected by read_mtx) ─────────────────────────────────────────
  std::mutex                                    read_mtx;
  std::condition_variable                       read_cv;
  std::queue<bison::buffer>                     inbox;
  std::unordered_map<uint64_t, partial_message> pending;

  // ── Atomics ───────────────────────────────────────────────────────────────
  std::atomic<bool>     closed{false};
  std::atomic<bool>     stop_requested{false};
  std::atomic<bool>     hello_seen{false};
  std::atomic<uint64_t> next_msg_id{1};

  // ── Configuration (write-once before reader thread starts) ────────────────
  size_t max_chunk_bytes = 1536U;
  size_t max_frame_bytes = 8U * 1024U * 1024U;
  std::chrono::milliseconds reassembly_timeout{5000};
  std::chrono::milliseconds handshake_timeout{300000};
};

// ── Low-level I/O helper ──────────────────────────────────────────────────

bool write_all_fd(int fd, const void* data, size_t size) {
  const auto* p = static_cast<const char*>(data);
  size_t sent = 0;
  while (sent < size) {
    const ssize_t n = ::write(fd, p + sent, size - sent);
    if (n > 0) {
      sent += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;
    return false;
  }
  return true;
}

// ── Base-64 codec ─────────────────────────────────────────────────────────

int from_b64(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

std::string b64_encode(const uint8_t* data, size_t size) {
  static constexpr char kAlpha[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((size + 2) / 3) * 4);
  for (size_t i = 0; i < size; i += 3) {
    const uint32_t b0 = data[i];
    const uint32_t b1 = (i + 1 < size) ? data[i + 1] : 0u;
    const uint32_t b2 = (i + 2 < size) ? data[i + 2] : 0u;
    const uint32_t n  = (b0 << 16) | (b1 << 8) | b2;
    out.push_back(kAlpha[(n >> 18) & 0x3f]);
    out.push_back(kAlpha[(n >> 12) & 0x3f]);
    out.push_back((i + 1 < size) ? kAlpha[(n >> 6) & 0x3f] : '=');
    out.push_back((i + 2 < size) ? kAlpha[n & 0x3f]        : '=');
  }
  return out;
}

bool b64_decode(const std::string& input, bison::buffer& out) {
  out.clear();
  if (input.empty()) return true;
  if ((input.size() % 4) != 0) return false;
  out.reserve((input.size() / 4) * 3);
  for (size_t i = 0; i < input.size(); i += 4) {
    const int v0 = from_b64(input[i]);
    const int v1 = from_b64(input[i + 1]);
    if (v0 < 0 || v1 < 0) return false;
    const bool p2 = (input[i + 2] == '=');
    const bool p3 = (input[i + 3] == '=');
    const int  v2 = p2 ? 0 : from_b64(input[i + 2]);
    const int  v3 = p3 ? 0 : from_b64(input[i + 3]);
    if ((!p2 && v2 < 0) || (!p3 && v3 < 0)) return false;
    const uint32_t n =
        (static_cast<uint32_t>(v0) << 18) |
        (static_cast<uint32_t>(v1) << 12) |
        (static_cast<uint32_t>(v2) << 6)  |
         static_cast<uint32_t>(v3);
    out.push_back(static_cast<uint8_t>((n >> 16) & 0xff));
    if (!p2) out.push_back(static_cast<uint8_t>((n >> 8) & 0xff));
    if (!p3) out.push_back(static_cast<uint8_t>(n & 0xff));
  }
  return true;
}

// ── Text helpers ──────────────────────────────────────────────────────────

std::optional<uint64_t> parse_u64(const std::string& v) {
  try {
    size_t pos = 0;
    const auto n = std::stoull(v, &pos, 10);
    if (pos != v.size()) return std::nullopt;
    return n;
  } catch (...) { return std::nullopt; }
}

std::optional<uint32_t> parse_u32(const std::string& v) {
  const auto n = parse_u64(v);
  if (!n || *n > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()))
    return std::nullopt;
  return static_cast<uint32_t>(*n);
}

std::unordered_map<std::string, std::string> parse_fields(
    const std::string& body) {
  std::unordered_map<std::string, std::string> fields;
  std::stringstream ss(body);
  std::string token;
  bool first = true;
  while (std::getline(ss, token, ';')) {
    if (first) {
      first = false;
      if (token != kProtoVersion) return {};
      continue;
    }
    const auto pos = token.find('=');
    if (pos == std::string::npos || pos == 0) continue;
    fields.emplace(token.substr(0, pos), token.substr(pos + 1));
  }
  return fields;
}

// ── Shared-state helpers ──────────────────────────────────────────────────

void push_inbound(client_state& state, bison::buffer frame) {
  {
    std::lock_guard<std::mutex> lk(state.read_mtx);
    state.inbox.push(std::move(frame));
  }
  state.read_cv.notify_all();
}

void close_and_notify(client_state& state) {
  state.closed.store(true);
  state.read_cv.notify_all();
}

void maybe_cleanup_expired(client_state& state) {
  const auto now = std::chrono::steady_clock::now();
  for (auto it = state.pending.begin(); it != state.pending.end();) {
    if (now - it->second.first_seen > state.reassembly_timeout)
      it = state.pending.erase(it);
    else
      ++it;
  }
}

void handle_data_frame(
    client_state& state,
    const std::unordered_map<std::string, std::string>& fields) {
  const auto it_id    = fields.find("id");
  const auto it_seq   = fields.find("seq");
  const auto it_total = fields.find("total");
  const auto it_b64   = fields.find("b64");

  if (it_id == fields.end() || it_seq == fields.end() ||
      it_total == fields.end() || it_b64 == fields.end())
    return;

  const auto id    = parse_u64(it_id->second);
  const auto seq   = parse_u32(it_seq->second);
  const auto total = parse_u32(it_total->second);

  if (!id || !seq || !total || *total == 0 || *seq >= *total)
    return;

  bison::buffer chunk;
  if (!b64_decode(it_b64->second, chunk))
    return;

  bison::buffer assembled;
  {
    std::unique_lock<std::mutex> lk(state.read_mtx);
    maybe_cleanup_expired(state);

    auto& pm = state.pending[*id];
    if (pm.total == 0) {
      pm.total      = *total;
      pm.parts.resize(*total);
      pm.collected  = 0;
      pm.first_seen = std::chrono::steady_clock::now();
    }
    if (pm.total != *total) {
      state.pending.erase(*id);
      return;
    }

    auto& slot = pm.parts[*seq];
    if (slot.has_value()) return;

    pm.collected += chunk.size();
    if (pm.collected > state.max_frame_bytes) {
      state.pending.erase(*id);
      return;
    }

    slot = std::move(chunk);

    for (const auto& part : pm.parts)
      if (!part.has_value()) return;

    assembled.reserve(pm.collected);
    for (auto& part : pm.parts)
      assembled.insert(assembled.end(), part->begin(), part->end());
    state.pending.erase(*id);
  }
  push_inbound(state, std::move(assembled));
}

void process_body(client_state& state, const std::string& body) {
  const auto fields = parse_fields(body);
  if (fields.empty()) return;

  const auto it_type = fields.find("type");
  if (it_type == fields.end()) return;

  if (it_type->second == kTypeHello) {
    state.hello_seen.store(true);
    state.read_cv.notify_all();
    return;
  }
  if (it_type->second == kTypeEnd) {
    close_and_notify(state);
    return;
  }
  if (it_type->second == kTypeData)
    handle_data_frame(state, fields);
}

// ── Frame emission ────────────────────────────────────────────────────────

void emit_dcs(client_state& state, const std::string& body) {
  std::lock_guard<std::mutex> lk(state.write_mtx);
  const std::string frame = std::string{kDcsStart} + body + kDcsEnd;
  if (!write_all_fd(state.write_fd, frame.data(), frame.size()))
    throw std::runtime_error("pty_client_transport: stdout write failed");
}

void emit_data(client_state& state, const bison::buffer& frame) {
  if (frame.size() > state.max_frame_bytes)
    throw std::runtime_error(
        "pty_client_transport: frame exceeds max_frame_bytes");

  const size_t   chunk = std::max<size_t>(1U, state.max_chunk_bytes);
  const uint64_t id    = state.next_msg_id.fetch_add(1);

  if (frame.empty()) {
    emit_dcs(state, std::string{kProtoVersion} +
                        ";type=DATA;id=" + std::to_string(id) +
                        ";seq=0;total=1;b64=");
    return;
  }

  const uint32_t total =
      static_cast<uint32_t>((frame.size() + chunk - 1) / chunk);

  for (uint32_t seq = 0; seq < total; ++seq) {
    const size_t off = static_cast<size_t>(seq) * chunk;
    const size_t len = std::min(chunk, frame.size() - off);
    const auto   b64 = b64_encode(frame.data() + off, len);
    emit_dcs(state, std::string{kProtoVersion} +
                        ";type=DATA;id="  + std::to_string(id) +
                        ";seq="           + std::to_string(seq) +
                        ";total="         + std::to_string(total) +
                        ";b64="           + b64);
  }
}

// ── Reader thread ─────────────────────────────────────────────────────────

/**
 * @brief Parse the stdin DCS stream and dispatch frames to the inbox.
 *
 * Non-DCS bytes on stdin (shell prompts, escape sequences) are silently
 * discarded — the client is not a terminal emulator.  Exits when
 * `stop_requested` is set or the channel reaches EOF.
 */
void client_reader_loop(std::shared_ptr<client_state> state) {
  if (!state) return;

  bool        in_dcs      = false;
  bool        saw_esc     = false;
  bool        dcs_saw_esc = false;
  std::string dcs_buf;

  while (!state->stop_requested.load()) {
    pollfd pfd{};
    pfd.fd     = state->read_fd;
    pfd.events = POLLIN;
    const int rc = ::poll(&pfd, 1, 100);
    if (rc == 0) continue;
    if (rc < 0) {
      if (errno == EINTR) continue;
      close_and_notify(*state);
      return;
    }
    if ((pfd.revents & POLLERR) != 0) {
      close_and_notify(*state);
      return;
    }

    uint8_t c = 0;
    const ssize_t n = ::read(state->read_fd, &c, 1);
    if (n == 0 || (n < 0 && errno != EINTR)) {
      close_and_notify(*state);
      return;
    }
    if (n < 0) continue; // EINTR

    const char ch = static_cast<char>(c);

    if (in_dcs) {
      if (dcs_saw_esc) {
        if (ch == '\\') {
          process_body(*state, dcs_buf);
          dcs_buf.clear();
          dcs_saw_esc = false;
          in_dcs      = false;
          continue;
        }
        dcs_buf.push_back(kEsc);
        dcs_saw_esc = false;
      }
      if (ch == kEsc) { dcs_saw_esc = true; continue; }
      dcs_buf.push_back(ch);
      continue;
    }

    if (saw_esc) {
      saw_esc = false;
      if (ch == 'P') {
        in_dcs = true;
        dcs_buf.clear();
        continue;
      }
      // Non-DCS ESC sequence — discard the ESC and fall through to discard ch.
    }

    if (ch == kEsc) { saw_esc = true; continue; }

    // Non-DCS plaintext byte: discard silently.
    (void)ch;
  }

  close_and_notify(*state);
}

} // namespace

// ── impl ──────────────────────────────────────────────────────────────────

struct pty_client_transport::impl {
  impl() : state(std::make_shared<client_state>()) {}

  std::shared_ptr<client_state> state;
  std::thread                   reader;
  bool                          opened          = false;
  bool                          reader_detached = false;
  bool                          tty_active      = false;
  termios                       saved_tty{};
};

// ── pty_client_transport ──────────────────────────────────────────────────

pty_client_transport::pty_client_transport()
    : impl_(std::make_unique<impl>()) {}

pty_client_transport::pty_client_transport(
    pty_client_transport&&) noexcept = default;
pty_client_transport& pty_client_transport::operator=(
    pty_client_transport&&) noexcept = default;

pty_client_transport::~pty_client_transport() {
  shutdown();
}

void pty_client_transport::open(bison::dynamic params) {
  if (!impl_ || !impl_->state)
    throw std::runtime_error("pty_client_transport::open: invalid state");
  if (impl_->reader_detached)
    throw std::runtime_error(
        "pty_client_transport::open: cannot reopen after shutdown");

  auto& st = *impl_->state;
  st.closed.store(false);
  st.stop_requested.store(false);
  st.hello_seen.store(false);

  // Apply params.
  if (const auto* f = params.findField("handshake_timeout_ms"_key);
      f != nullptr && f->is<int32_t>())
    st.handshake_timeout = std::chrono::milliseconds{std::max(0, f->as<int32_t>())};

  // Put stdin into raw/no-echo mode so that:
  // - Server DCS frames (written to PTY master → slave stdin) are delivered
  //   immediately without waiting for a newline (ICANON=0).
  // - Those same frames are not echoed back onto the terminal (ECHO=0).
  if (::isatty(STDIN_FILENO)) {
    termios tty{};
    if (::tcgetattr(STDIN_FILENO, &tty) == 0) {
      impl_->saved_tty    = tty;
      impl_->tty_active   = true;
      tty.c_lflag &= static_cast<tcflag_t>(
          ~(ICANON | ECHO | ECHOE | ECHOK | ECHONL | ISIG | ECHOCTL));
      tty.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL | BRKINT));
      tty.c_cc[VMIN]  = 1;
      tty.c_cc[VTIME] = 0;
      (void)::tcsetattr(STDIN_FILENO, TCSANOW, &tty);
    }
  }

  // Start reader thread before emitting HELLO so we do not miss the server's
  // response if it arrives very quickly.
  if (!impl_->reader.joinable())
    impl_->reader = std::thread(client_reader_loop, impl_->state);

  // Client initiates the handshake by sending HELLO first.
  emit_dcs(st, std::string{kProtoVersion} + ";type=HELLO");

  // Wait for the server's HELLO response.
  {
    std::unique_lock<std::mutex> lk(st.read_mtx);
    const bool ok = st.read_cv.wait_for(lk, st.handshake_timeout, [&st] {
      return st.hello_seen.load() || st.closed.load();
    });
    if (!ok || !st.hello_seen.load())
      throw std::runtime_error(
          "pty_client_transport::open: handshake timeout");
  }

  impl_->opened = true;
}

void pty_client_transport::send(bison::buffer frame) {
  if (!impl_ || !impl_->state || !impl_->opened)
    throw std::runtime_error("pty_client_transport::send: not open");
  if (impl_->state->closed.load())
    throw std::runtime_error("pty_client_transport::send: closed");
  emit_data(*impl_->state, frame);
}

bool pty_client_transport::receive(
    bison::buffer& frame,
    std::chrono::milliseconds timeout) {
  if (!impl_ || !impl_->state || !impl_->opened)
    return false;

  auto& st = *impl_->state;
  std::unique_lock<std::mutex> lk(st.read_mtx);
  if (!st.read_cv.wait_for(lk, timeout, [&st] {
        return !st.inbox.empty() || st.closed.load();
      }))
    return false;

  if (st.inbox.empty()) return false;

  frame = std::move(st.inbox.front());
  st.inbox.pop();
  return true;
}

void pty_client_transport::shutdown() {
  if (!impl_ || !impl_->state) return;
  auto& st = *impl_->state;

  if (impl_->opened && !st.closed.load()) {
    try {
      emit_dcs(st, std::string{kProtoVersion} + ";type=END");
    } catch (...) {}
  }

  st.stop_requested.store(true);
  st.closed.store(true);
  st.read_cv.notify_all();

  if (impl_->reader.joinable()) {
    impl_->reader.detach();
    impl_->reader_detached = true;
  }

  if (impl_->tty_active) {
    (void)::tcsetattr(STDIN_FILENO, TCSANOW, &impl_->saved_tty);
    impl_->tty_active = false;
  }

  impl_->opened = false;
}

} // namespace bdg::bison::pty

#endif // defined(__linux__)

// MIT License © 2025 Binary Dice Games
/**
 * @file pty_server_transport.cpp
 * @brief PTY-owning server transport implementation.
 *
 * DCS byte-level state machine with PTY-specific routing: plaintext bytes go
 * to `stdout` (so the user sees the
 * shell), while DCS frames are queued for the bison RMI layer.  All writes to
 * the PTY master fd are serialised by `pty_shared_state::write_mtx` so the
 * input-relay thread and `pty_server_connection::send()` never interleave.
 */
#include "src/app/pty/pty_server_transport.hpp"

#if defined(__linux__)

#include <poll.h>
#include <pty.h>
#include <sys/wait.h>
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

namespace bdg::bison::app {

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

/** @brief Accumulator for one in-flight multi-chunk DATA message. */
struct partial_message {
  uint32_t total = 0;
  std::vector<std::optional<bison::buffer>> parts;
  size_t collected = 0;
  std::chrono::steady_clock::time_point first_seen;
};

// ── Shared session state ──────────────────────────────────────────────────

/**
 * @brief All mutable state shared between `pty_server_transport` and
 *        `pty_server_connection`.
 *
 * ### Locking rules
 * - `write_mtx`  — held for every write to `master_fd`; shared between the
 *                   input-relay thread and `pty_server_connection::send()`.
 * - `read_mtx`   — protects `inbox` and `pending`; transitions notify `read_cv`.
 * - `closed`, `hello_seen`, `shell_running`, `stop_requested` — atomics,
 *   readable from any thread without holding a mutex.
 * - All other fields are write-once before threads start and treated as
 *   read-only thereafter.
 */
struct pty_shared_state {
  // ── I/O ───────────────────────────────────────────────────────────────────
  int master_fd = -1;

  // ── Write lock ────────────────────────────────────────────────────────────
  std::mutex write_mtx;

  // ── Inbox (protected by read_mtx) ─────────────────────────────────────────
  std::mutex                                    read_mtx;
  std::condition_variable                       read_cv;
  std::queue<bison::buffer>                     inbox;
  std::unordered_map<uint64_t, partial_message> pending;

  // ── Session atomics (reset by restart_session()) ──────────────────────────
  std::atomic<bool> closed{false};
  std::atomic<bool> hello_seen{false};

  // ── Transport-level atomics ───────────────────────────────────────────────
  std::atomic<bool>     shell_running{false};
  std::atomic<bool>     stop_requested{false};
  std::atomic<uint64_t> next_msg_id{1};

  // ── Configuration (set once before threads start) ─────────────────────────
  size_t max_chunk_bytes = 1536U;
  size_t max_frame_bytes = 8U * 1024U * 1024U;
  std::chrono::milliseconds reassembly_timeout{5000};
};

// ── Low-level I/O helper ──────────────────────────────────────────────────

/** @brief Write all @p size bytes from @p data to @p fd, retrying on EINTR. */
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

/** @brief Decode one RFC 4648 base64 character; returns -1 for invalid input. */
int from_b64(char c) {
  if (c >= 'A' && c <= 'Z')
    return c - 'A';
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 26;
  if (c >= '0' && c <= '9')
    return c - '0' + 52;
  if (c == '+')
    return 62;
  if (c == '/')
    return 63;
  return -1;
}

/** @brief Encode @p size bytes at @p data as RFC 4648 base64. */
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

/**
 * @brief Decode RFC 4648 base64 @p input into @p out.
 * @return `true` on success; `false` on malformed input.
 */
bool b64_decode(const std::string& input, bison::buffer& out) {
  out.clear();
  if (input.empty())
    return true;
  if ((input.size() % 4) != 0)
    return false;
  out.reserve((input.size() / 4) * 3);
  for (size_t i = 0; i < input.size(); i += 4) {
    const int v0 = from_b64(input[i]);
    const int v1 = from_b64(input[i + 1]);
    if (v0 < 0 || v1 < 0)
      return false;
    const bool p2 = (input[i + 2] == '=');
    const bool p3 = (input[i + 3] == '=');
    const int  v2 = p2 ? 0 : from_b64(input[i + 2]);
    const int  v3 = p3 ? 0 : from_b64(input[i + 3]);
    if ((!p2 && v2 < 0) || (!p3 && v3 < 0))
      return false;
    const uint32_t n =
        (static_cast<uint32_t>(v0) << 18) |
        (static_cast<uint32_t>(v1) << 12) |
        (static_cast<uint32_t>(v2) << 6)  |
         static_cast<uint32_t>(v3);
    out.push_back(static_cast<uint8_t>((n >> 16) & 0xff));
    if (!p2)
      out.push_back(static_cast<uint8_t>((n >> 8) & 0xff));
    if (!p3)
      out.push_back(static_cast<uint8_t>(n & 0xff));
  }
  return true;
}

// ── Text helpers ──────────────────────────────────────────────────────────

std::string trim_copy(const std::string& s) {
  size_t b = 0;
  while (b < s.size() &&
         std::isspace(static_cast<unsigned char>(s[b])))
    ++b;
  size_t e = s.size();
  while (e > b &&
         std::isspace(static_cast<unsigned char>(s[e - 1])))
    --e;
  return s.substr(b, e - b);
}

std::optional<uint64_t> parse_u64(const std::string& v) {
  try {
    size_t pos = 0;
    const auto n = std::stoull(v, &pos, 10);
    if (pos != v.size())
      return std::nullopt;
    return n;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<uint32_t> parse_u32(const std::string& v) {
  const auto n = parse_u64(v);
  if (!n || *n > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()))
    return std::nullopt;
  return static_cast<uint32_t>(*n);
}

/**
 * @brief Parse a semicolon-delimited control-string body into key-value pairs.
 *
 * The first token must be `BISON_RMI/1`; subsequent tokens are `key=value`.
 * Returns an empty map if the version token is missing or wrong.
 */
std::unordered_map<std::string, std::string> parse_fields(
    const std::string& body) {
  std::unordered_map<std::string, std::string> fields;
  std::stringstream ss(body);
  std::string token;
  bool first = true;
  while (std::getline(ss, token, ';')) {
    if (first) {
      first = false;
      if (token != kProtoVersion)
        return {};
      continue;
    }
    const auto pos = token.find('=');
    if (pos == std::string::npos || pos == 0)
      continue;
    fields.emplace(token.substr(0, pos), token.substr(pos + 1));
  }
  return fields;
}

// ── Shared-state helpers ──────────────────────────────────────────────────

void push_inbound(pty_shared_state& state, bison::buffer frame) {
  {
    std::lock_guard<std::mutex> lk(state.read_mtx);
    state.inbox.push(std::move(frame));
  }
  state.read_cv.notify_all();
}

void close_and_notify(pty_shared_state& state) {
  state.closed.store(true);
  state.read_cv.notify_all();
}

void maybe_cleanup_expired(pty_shared_state& state) {
  const auto now = std::chrono::steady_clock::now();
  for (auto it = state.pending.begin(); it != state.pending.end();) {
    if (now - it->second.first_seen > state.reassembly_timeout)
      it = state.pending.erase(it);
    else
      ++it;
  }
}

void handle_data_frame(
    pty_shared_state& state,
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
    if (slot.has_value())
      return;

    pm.collected += chunk.size();
    if (pm.collected > state.max_frame_bytes) {
      state.pending.erase(*id);
      return;
    }

    slot = std::move(chunk);

    for (const auto& part : pm.parts)
      if (!part.has_value())
        return;

    assembled.reserve(pm.collected);
    for (auto& part : pm.parts)
      assembled.insert(assembled.end(), part->begin(), part->end());
    state.pending.erase(*id);
  }
  push_inbound(state, std::move(assembled));
}

/** @brief Dispatch a decoded DCS body to HELLO, END, or DATA handlers. */
void process_body(pty_shared_state& state, const std::string& body) {
  const auto fields = parse_fields(body);
  if (fields.empty())
    return;
  const auto it_type = fields.find("type");
  if (it_type == fields.end())
    return;

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

/** @brief Write one DCS frame (ESC P <body> ESC \) to the PTY master. */
void emit_dcs(pty_shared_state& state, const std::string& body) {
  std::lock_guard<std::mutex> lk(state.write_mtx);
  const std::string frame = std::string{kDcsStart} + body + kDcsEnd;
  if (!write_all_fd(state.master_fd, frame.data(), frame.size()))
    throw std::runtime_error("pty_server_transport: PTY write failed");
}

/**
 * @brief Split @p frame into chunks, base64-encode them, and emit DATA frames.
 *
 * Each chunk is sent as a separate DCS frame.  An empty @p frame is sent as a
 * single chunk with `b64=` so the receiver delivers a zero-byte payload rather
 * than silently dropping the message.
 */
void emit_data(pty_shared_state& state, const bison::buffer& frame) {
  if (frame.size() > state.max_frame_bytes)
    throw std::runtime_error(
        "pty_server_transport: frame exceeds max_frame_bytes");

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
 * @brief Background thread: read from PTY master and demultiplex the stream.
 *
 * Runs the DCS state machine: bytes outside DCS blocks are written to
 * `stdout` (so the user sees the shell normally); DCS blocks are decoded and
 * dispatched as bison frames.  Exits on `stop_requested` or PTY EOF.
 *
 * Plaintext is buffered up to 4 KiB before flushing to stdout to reduce the
 * number of `write` syscalls.  The buffer is always flushed before a DCS
 * block is processed so the output order is preserved.
 */
void reader_loop(std::shared_ptr<pty_shared_state> state) {
  if (!state || state->master_fd < 0)
    return;

  bool        in_dcs      = false;
  bool        saw_esc     = false;
  bool        dcs_saw_esc = false;
  std::string dcs_buf;
  std::vector<uint8_t> plain_buf;
  plain_buf.reserve(4096);

  const auto flush_plain = [&]() {
    if (!plain_buf.empty()) {
      (void)write_all_fd(STDOUT_FILENO, plain_buf.data(), plain_buf.size());
      plain_buf.clear();
    }
  };

  while (!state->stop_requested.load()) {
    // Poll with a short timeout so buffered plaintext is flushed when the
    // shell is idle (e.g., waiting at a prompt between bursts of output).
    pollfd pfd{};
    pfd.fd     = state->master_fd;
    pfd.events = POLLIN;
    const int rc = ::poll(&pfd, 1, 10);
    if (rc == 0) {
      flush_plain();
      continue;
    }
    if (rc < 0) {
      if (errno == EINTR) continue;
      flush_plain();
      state->shell_running.store(false);
      close_and_notify(*state);
      return;
    }
    if ((pfd.revents & POLLERR) != 0) {
      flush_plain();
      state->shell_running.store(false);
      close_and_notify(*state);
      return;
    }

    uint8_t c = 0;
    const ssize_t n = ::read(state->master_fd, &c, 1);
    if (n == 0 || (n < 0 && errno != EINTR)) {
      flush_plain();
      state->shell_running.store(false);
      close_and_notify(*state);
      return;
    }
    if (n < 0)
      continue; // EINTR

    const char ch = static_cast<char>(c);

    // ── DCS body accumulator ──────────────────────────────────────────────
    if (in_dcs) {
      if (dcs_saw_esc) {
        if (ch == '\\') {
          // ESC \ is the DCS String Terminator.
          flush_plain();
          process_body(*state, dcs_buf);
          dcs_buf.clear();
          dcs_saw_esc = false;
          in_dcs      = false;
          continue;
        }
        // ESC not followed by \ — emit the literal ESC into the DCS body.
        dcs_buf.push_back(kEsc);
        dcs_saw_esc = false;
      }
      if (ch == kEsc) {
        dcs_saw_esc = true;
        continue;
      }
      dcs_buf.push_back(ch);
      continue;
    }

    // ── ESC lookahead (outside DCS) ───────────────────────────────────────
    if (saw_esc) {
      saw_esc = false;
      if (ch == 'P') {
        // ESC P opens a DCS block.
        flush_plain();
        in_dcs = true;
        dcs_buf.clear();
        continue;
      }
      // Not a recognised sequence — treat the ESC as a plaintext byte.
      plain_buf.push_back(static_cast<uint8_t>(kEsc));
    }

    if (ch == kEsc) {
      saw_esc = true;
      continue;
    }

    // ── Plaintext ─────────────────────────────────────────────────────────
    plain_buf.push_back(c);
    if (plain_buf.size() >= 4096)
      flush_plain();
  }

  flush_plain();
  close_and_notify(*state);
}

// ── Input relay thread ────────────────────────────────────────────────────

/**
 * @brief Background thread: relay user keystrokes from `stdin` to the PTY.
 *
 * Polls `stdin` with a 100 ms timeout so it can exit promptly when
 * `stop_requested` is set.  All writes to `master_fd` go through `write_mtx`
 * to serialise with outgoing bison frames.
 */
void input_relay_loop(std::shared_ptr<pty_shared_state> state) {
  if (!state)
    return;

  char buf[256];
  while (!state->stop_requested.load()) {
    pollfd pfd{};
    pfd.fd     = STDIN_FILENO;
    pfd.events = POLLIN;

    const int rc = ::poll(&pfd, 1, 100);
    if (rc == 0)
      continue;
    if (rc < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if ((pfd.revents & (POLLERR | POLLNVAL)) != 0)
      break;
    if ((pfd.revents & (POLLIN | POLLHUP)) == 0)
      continue;

    const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
    if (n > 0) {
      std::lock_guard<std::mutex> lk(state->write_mtx);
      if (state->master_fd >= 0)
        (void)write_all_fd(state->master_fd, buf, static_cast<size_t>(n));
    } else if (n == 0 || (n < 0 && errno != EINTR)) {
      break;
    }
  }
}

} // namespace

// ── impl structs ──────────────────────────────────────────────────────────

struct pty_server_connection::impl {
  explicit impl(std::shared_ptr<pty_shared_state> s) : state(std::move(s)) {}
  std::shared_ptr<pty_shared_state> state;
};

struct pty_server_transport::impl {
  explicit impl(std::string shell)
      : shell_(std::move(shell)),
        state_(std::make_shared<pty_shared_state>()) {}

  std::string                       shell_;
  pid_t                             shell_pid_{-1};
  std::thread                       reader_thread_;
  std::thread                       input_relay_thread_;
  std::shared_ptr<pty_shared_state> state_;
  std::atomic<bool>                 started_{false};
  std::atomic<bool>                 accepted_{false};
  bool                              tty_active_{false};
  termios                           saved_tty_{};
};

// ══════════════════════════════════════════════════════════════════════════════
// pty_server_connection
// ══════════════════════════════════════════════════════════════════════════════

pty_server_connection::pty_server_connection(std::unique_ptr<impl> impl)
    : impl_(std::move(impl)) {}

pty_server_connection::~pty_server_connection() {
  close();
}

pty_server_connection::pty_server_connection(
    pty_server_connection&&) noexcept = default;
pty_server_connection& pty_server_connection::operator=(
    pty_server_connection&&) noexcept = default;

void pty_server_connection::send(bison::buffer frame) {
  if (!impl_ || !impl_->state)
    throw std::runtime_error("pty_server_connection::send: no state");
  if (impl_->state->closed.load())
    throw std::runtime_error("pty_server_connection::send: closed");
  emit_data(*impl_->state, frame);
}

bool pty_server_connection::receive(
    bison::buffer& frame,
    std::chrono::milliseconds timeout) {
  if (!impl_ || !impl_->state)
    return false;
  auto& st = *impl_->state;
  std::unique_lock<std::mutex> lk(st.read_mtx);
  if (!st.read_cv.wait_for(lk, timeout, [&st] {
        return !st.inbox.empty() || st.closed.load();
      }))
    return false;
  if (st.inbox.empty())
    return false;
  frame = std::move(st.inbox.front());
  st.inbox.pop();
  return true;
}

void pty_server_connection::close() {
  if (!impl_ || !impl_->state)
    return;
  close_and_notify(*impl_->state);
}

bool pty_server_connection::is_closed() const {
  return !impl_ || !impl_->state || impl_->state->closed.load();
}

// ══════════════════════════════════════════════════════════════════════════════
// pty_server_transport
// ══════════════════════════════════════════════════════════════════════════════

pty_server_transport::pty_server_transport(std::string shell)
    : impl_(std::make_unique<impl>(std::move(shell))) {}

pty_server_transport::~pty_server_transport() {
  stop();
}

void pty_server_transport::start(bison::dynamic params) {
  (void)params; // mode=dcs is forced; no other params apply

  if (impl_->started_.exchange(true))
    return; // idempotent

  int master_fd = -1;
  winsize ws{};
  ws.ws_col = 500;
  ws.ws_row = 50;
  const pid_t pid = ::forkpty(&master_fd, nullptr, nullptr, &ws);
  if (pid < 0) {
    impl_->started_.store(false);
    throw std::runtime_error("pty_server_transport::start: forkpty failed");
  }

  if (pid == 0) {
    // Child process: exec the shell.
    ::execlp(impl_->shell_.c_str(), impl_->shell_.c_str(), nullptr);
    _exit(127);
  }

  // Parent: set up shared state.
  impl_->shell_pid_          = pid;
  impl_->state_->master_fd   = master_fd;
  impl_->state_->shell_running.store(true);
  impl_->state_->stop_requested.store(false);

  // Switch user's terminal to raw / no-echo mode.
  if (::isatty(STDIN_FILENO)) {
    termios tty{};
    if (::tcgetattr(STDIN_FILENO, &tty) == 0) {
      impl_->saved_tty_ = tty;
      tty.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
      tty.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL));
      tty.c_cc[VMIN]  = 1;
      tty.c_cc[VTIME] = 0;
      if (::tcsetattr(STDIN_FILENO, TCSANOW, &tty) == 0)
        impl_->tty_active_ = true;
    }
  }

  // Start reader thread before relay so early client frames are not lost.
  impl_->reader_thread_ = std::thread(reader_loop, impl_->state_);
  // Start input relay last — user input should not race reader startup.
  impl_->input_relay_thread_ = std::thread(input_relay_loop, impl_->state_);
}

std::unique_ptr<rmi::transport::server_connection_iface>
pty_server_transport::accept(std::chrono::milliseconds timeout) {
  auto& st = *impl_->state_;

  // If already accepted and session not yet reset, block and return nullptr.
  if (impl_->accepted_.exchange(true)) {
    std::unique_lock<std::mutex> lk(st.read_mtx);
    st.read_cv.wait_for(lk, timeout, [&st] {
      return st.closed.load() || !st.shell_running.load();
    });
    return nullptr;
  }

  // Wait for HELLO from the client, or shell exit.
  {
    std::unique_lock<std::mutex> lk(st.read_mtx);
    const bool ok = st.read_cv.wait_for(lk, timeout, [&st] {
      return st.hello_seen.load() || !st.shell_running.load();
    });
    if (!ok || !st.hello_seen.load()) {
      // Timeout or shell exited without a HELLO — reset so the app can retry.
      impl_->accepted_.store(false);
      return nullptr;
    }
  }

  // Respond to the client's HELLO so the client's open() handshake completes.
  emit_dcs(*impl_->state_, std::string{kProtoVersion} + ";type=HELLO");

  return std::make_unique<pty_server_connection>(
      std::make_unique<pty_server_connection::impl>(impl_->state_));
}

void pty_server_transport::stop() {
  if (!impl_->started_.load())
    return;

  auto& st = *impl_->state_;

  // Ask the shell to exit gracefully before closing the fd.
  if (st.master_fd >= 0) {
    std::lock_guard<std::mutex> lk(st.write_mtx);
    const char exit_cmd[] = "exit\n";
    (void)write_all_fd(st.master_fd, exit_cmd, sizeof(exit_cmd) - 1);
  }

  st.stop_requested.store(true);
  st.shell_running.store(false);
  close_and_notify(st);

  // Close PTY master — signals EOF to the shell subprocess.
  if (st.master_fd >= 0) {
    ::close(st.master_fd);
    st.master_fd = -1;
  }

  // Restore the user's terminal mode before detaching threads.
  if (impl_->tty_active_) {
    (void)::tcsetattr(STDIN_FILENO, TCSANOW, &impl_->saved_tty_);
    impl_->tty_active_ = false;
  }

  // Detach both background threads — they may be blocked in read()/poll().
  if (impl_->reader_thread_.joinable())
    impl_->reader_thread_.detach();
  if (impl_->input_relay_thread_.joinable())
    impl_->input_relay_thread_.detach();

  // Reap the shell subprocess.
  if (impl_->shell_pid_ > 0) {
    int status = 0;
    (void)::waitpid(impl_->shell_pid_, &status, 0);
    impl_->shell_pid_ = -1;
  }
}

void pty_server_transport::restart_session() {
  auto& st = *impl_->state_;
  {
    std::lock_guard<std::mutex> lk(st.read_mtx);
    st.inbox   = std::queue<bison::buffer>{};
    st.pending.clear();
  }
  st.closed.store(false);
  st.hello_seen.store(false);
  impl_->accepted_.store(false);
  // No HELLO emission here — the next client initiates by sending its own
  // HELLO, and accept() responds when it arrives.
}

bool pty_server_transport::is_shell_running() const {
  return impl_->state_->shell_running.load();
}

bool pty_server_transport::wait_until_closed(
    std::chrono::milliseconds timeout) const {
  auto& st = *impl_->state_;
  std::unique_lock<std::mutex> lk(st.read_mtx);
  return st.read_cv.wait_for(lk, timeout, [&st] {
    return st.closed.load() || !st.shell_running.load();
  });
}

} // namespace bdg::bison::app

#endif // defined(__linux__)

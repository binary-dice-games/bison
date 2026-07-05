// MIT License © 2025 Binary Dice Games
/**
 * @file term_transport.cpp
 * @brief libuv-backed implementation of the term transport, which frames
 *        client->server envelopes as OSC-99 sequences and server->client
 *        envelopes as `BISON<...>` markers (see term_role's doc comment for
 *        why the two directions need different wire formats).
 *        See term_transport.hpp and FORMAT.md §5.3 for the framing contract.
 */
#include "src/rmi/transport/term_transport.hpp"
#include "src/rmi/transport/fd_dup.hpp"
#include "src/rmi/transport/uv_stream_state.hpp"

#include <gflags/gflags.h>
#include <uv.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// Temporary debug aid: path to append term_transport diagnostic trace logs
// to (handle setup, handshake progress, raw read/write activity). Empty
// (the default) disables tracing entirely. Used to diagnose ConPTY/console
// interop issues that don't reproduce under Linux/MSYS2 and are hard to
// observe interactively since the process under test owns the console.
DEFINE_string(
    trace,
    "C:\\tmp\\bison.log",
    "Path to append term_transport diagnostic trace logs to; empty disables tracing");

namespace bdg::bison::rmi::transport {

namespace {

bison::synchronized<std::ofstream>& trace_file() {
  static bison::synchronized<std::ofstream> instance;
  return instance;
}

bool trace_open() {
  static const bool opened = [] {
    if (FLAGS_trace.empty())
      return false;
    trace_file().wlock()->open(FLAGS_trace, std::ios::app);
    return trace_file().rlock()->is_open();
  }();
  return opened;
}

void trace(std::string_view tag, const std::string& msg) {
  if (!trace_open())
    return;
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  trace_file().withWLock([&](std::ofstream& f) { f << ms << " [" << tag << "] " << msg << std::endl; });
}

std::string hex_preview(const uint8_t* data, size_t len) {
  static constexpr size_t kMax = 48;
  std::ostringstream oss;
  oss << "len=" << len << " bytes=\"";
  for (size_t i = 0; i < std::min(len, kMax); ++i) {
    const uint8_t b = data[i];
    if (b >= 0x20 && b < 0x7f)
      oss << static_cast<char>(b);
    else
      oss << "\\x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b) << std::dec;
  }
  if (len > kMax)
    oss << "...";
  oss << "\"";
  return oss.str();
}

// ── Base64 (duplicated from stdio_transport.cpp's anonymous-namespace helper
//    of the same name — not shared across translation units on purpose, to
//    keep this file's framing changes isolated from that stable one) ────────

constexpr char kBase64Chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const bison::buffer& in) {
  std::string out;
  out.reserve(((in.size() + 2) / 3) * 4);
  size_t i = 0;
  while (i + 3 <= in.size()) {
    const uint32_t n = (static_cast<uint32_t>(in[i]) << 16) | (static_cast<uint32_t>(in[i + 1]) << 8) |
        static_cast<uint32_t>(in[i + 2]);
    out.push_back(kBase64Chars[(n >> 18) & 0x3F]);
    out.push_back(kBase64Chars[(n >> 12) & 0x3F]);
    out.push_back(kBase64Chars[(n >> 6) & 0x3F]);
    out.push_back(kBase64Chars[n & 0x3F]);
    i += 3;
  }
  const size_t rem = in.size() - i;
  if (rem == 1) {
    const uint32_t n = static_cast<uint32_t>(in[i]) << 16;
    out.push_back(kBase64Chars[(n >> 18) & 0x3F]);
    out.push_back(kBase64Chars[(n >> 12) & 0x3F]);
    out.push_back('=');
    out.push_back('=');
  } else if (rem == 2) {
    const uint32_t n = (static_cast<uint32_t>(in[i]) << 16) | (static_cast<uint32_t>(in[i + 1]) << 8);
    out.push_back(kBase64Chars[(n >> 18) & 0x3F]);
    out.push_back(kBase64Chars[(n >> 12) & 0x3F]);
    out.push_back(kBase64Chars[(n >> 6) & 0x3F]);
    out.push_back('=');
  }
  return out;
}

constexpr int8_t kDecodeTableSize = 80; // '+' (0x2B) through 'z' (0x7A)

std::array<int8_t, kDecodeTableSize> make_decode_table() {
  std::array<int8_t, kDecodeTableSize> table{};
  table.fill(-1);
  for (int8_t i = 0; i < 64; ++i)
    table[static_cast<unsigned char>(kBase64Chars[i]) - '+'] = i;
  return table;
}

std::optional<bison::buffer> base64_decode(std::string_view in) {
  static const auto table = make_decode_table();
  if (in.empty())
    return bison::buffer{};
  if (in.size() % 4 != 0)
    return std::nullopt;

  bison::buffer out;
  out.reserve((in.size() / 4) * 3);

  for (size_t i = 0; i < in.size(); i += 4) {
    int32_t vals[4];
    int pad = 0;
    for (int j = 0; j < 4; ++j) {
      const unsigned char c = static_cast<unsigned char>(in[i + j]);
      if (c == '=') {
        vals[j] = 0;
        ++pad;
      } else {
        if (c < '+' || c >= '+' + kDecodeTableSize || table[c - '+'] < 0)
          return std::nullopt;
        if (pad > 0)
          return std::nullopt;
        vals[j] = table[c - '+'];
      }
    }
    if (pad > 2 || (pad > 0 && i + 4 != in.size()))
      return std::nullopt;

    const uint32_t n = (static_cast<uint32_t>(vals[0]) << 18) | (static_cast<uint32_t>(vals[1]) << 12) |
        (static_cast<uint32_t>(vals[2]) << 6) | static_cast<uint32_t>(vals[3]);
    out.push_back(static_cast<uint8_t>((n >> 16) & 0xFF));
    if (pad < 2)
      out.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
    if (pad < 1)
      out.push_back(static_cast<uint8_t>(n & 0xFF));
  }
  return out;
}

bool parse_uint(std::string_view s, uint32_t& out) {
  if (s.empty() || s.size() > 10)
    return false;
  uint64_t v = 0;
  for (const char c : s) {
    if (c < '0' || c > '9')
      return false;
    v = v * 10 + static_cast<uint64_t>(c - '0');
    if (v > 0xFFFFFFFFULL)
      return false;
  }
  out = static_cast<uint32_t>(v);
  return true;
}

// ── Wire role: which framing to use in which direction ──────────────────────
//
// ConPTY's two pipes are not symmetric the way a POSIX pty's master/slave
// is. Data flowing client-stdout -> ConPTY's *output* pipe -> server-read
// passes through ConPTY's output engine, which is built to shepherd OSC
// escape sequences through as atomic, opaque units (see the OSC-99 framing
// constants below and term_transport.hpp's doc comment) -- that direction
// is safe for OSC-99. Data flowing server-write -> ConPTY's *input* pipe ->
// client-stdin is not a passthrough channel: ConPTY runs it through a VT
// *input* state machine that only recognizes a small fixed table of real
// input sequences (arrow/function keys, mouse, bracketed paste, focus) to
// synthesize keyboard INPUT_RECORDs for the child process -- there is no
// "pass unrecognized bytes through verbatim" contract there, so an OSC-99
// sequence written into it is silently absorbed and never reaches the
// client as literal bytes. This exact asymmetry is documented in this
// repo's src/term/ANALYSIS.md §2, which uses plain literal text for the
// parent->child direction for the same reason. So: the client role sends
// OSC-99 (client-stdout direction) and receives the marker format
// (client-stdin direction); the server role does the reverse.
enum class term_role { client, server };

// ── OSC-99 framing constants (client -> server direction) ───────────────────

constexpr std::string_view kOscPrefix = "\x1b]99;";
constexpr char kOscTerminator = '\x07';

// Fixed per-sequence overhead ("\x1b]99;" + ';' + ';' + BEL) plus a generous
// budget for the decimal <seq>/<total> fields (10 digits each covers well
// beyond any envelope count this transport will ever see), subtracted from
// kMaxOscSequenceBytes to get how many base64 characters — and therefore how
// many raw payload bytes — one chunk may carry. See term_transport.hpp's
// doc comment for why this cap exists (ConPTY/pty relaying reliability).
constexpr size_t kOscFixedOverhead = kOscPrefix.size() + 1 + 1 + 1;
constexpr size_t kSeqDigitBudget = 10;
constexpr size_t kBase64Budget = kMaxOscSequenceBytes - kOscFixedOverhead - 2 * kSeqDigitBudget;
constexpr size_t kChunkRawBytes = (kBase64Budget / 4) * 3;
static_assert(kChunkRawBytes > 0, "kMaxOscSequenceBytes is too small to fit any chunk payload");

// ── Marker framing constants (server -> client direction) ───────────────────
//
// Reuses stdio_transport.cpp's exact BISON<...> wire text (duplicated, same
// rationale as the base64 helpers above): plain ASCII, no ESC byte anywhere,
// so it can never be mis-parsed as a terminal input control sequence, and
// ConPTY delivers it to the child exactly like typed characters. One marker
// per whole envelope, no chunking -- matching stdio_transport's precedent,
// since the kMaxOscSequenceBytes safety cap is specific to escape-sequence
// relaying and doesn't apply here.

constexpr std::string_view kMarkerPrefix = "BISON<";
constexpr char kMarkerSuffix = '>';

// Bound on how long a capturing-but-unterminated OSC-99 body is allowed to
// grow before it's treated as corrupt and discarded — well above what any
// legitimate chunk (bounded by kMaxOscSequenceBytes) could ever produce, so
// this only ever trips on a genuinely malformed/truncated sequence.
constexpr size_t kMaxCaptureBytes = kMaxOscSequenceBytes * 4;

// ── Connect-time handshake (verbatim reuse of stdio_transport's wire text —
//    see its doc comment; duplicated here rather than shared across
//    translation units, same rationale as the base64 helpers above) ─────────

constexpr std::string_view kHandshakeStart = "START BISON/1.0\r\n";
constexpr std::string_view kHandshakeOk = "BISON/1.0 OK\r\n";
constexpr std::string_view kHandshakeStop = "STOP BISON/1.0\r\n";
constexpr size_t kHandshakeAccumCap = 256;

// Token-only (no trailing "\r\n") match used solely by the server's
// watch_for_handshake_start(), below. The server's read side is the spawned
// shell's conout, which the shell itself (and any other child processes)
// also write to -- e.g. cmd.exe emits its own `ESC ] 0 ; <title> BEL`
// window-title updates on that same stream. Those writes can interleave
// with the client's "START BISON/1.0\r\n" write at an arbitrary byte
// boundary, so the trailing "\r\n" is not guaranteed to arrive contiguous
// with the token; requiring the full literal (including "\r\n") caused
// intermittent handshake-timeout failures when a title update landed
// between the "\r" and the "\n". The client's own accum.find(kHandshakeOk)
// below has no such risk: conin is written only by this transport's peer,
// never shared with another process.
constexpr std::string_view kHandshakeStartToken = kHandshakeStart.substr(0, kHandshakeStart.size() - 2);

} // namespace

// ── term_pipe_thread: shared loop/thread lifecycle ──────────────────────────

/**
 * @brief Owns one libuv loop, one stream handle wrapping a duplicate of a
 *        caller-supplied fd, and its thread.
 *
 * The wrapped fd is usually a real pipe (a `terminal`'s ConPTY/pty I/O
 * handle), but when this process is itself running attached to a console
 * (e.g. a client launched directly inside the shell a `--transport=term`
 * server spawned), fd 0/1 are a console handle instead. `uv_pipe_open()`
 * only accepts real pipe/disk-file handles on native Windows — it rejects
 * console handles outright — so `uv_guess_handle()` picks the right libuv
 * stream constructor (`uv_pipe_open` vs `uv_tty_init`) at runtime. This is
 * not a platform branch: `uv_guess_handle()` returns the correct answer on
 * every platform, and POSIX's `uv_pipe_open()` already happens to accept
 * tty fds too, so this only changes behavior where the old code failed
 * outright (a Windows console handle).
 */
struct term_pipe_thread {
  uv_loop_t loop{};
  uv_any_handle handle{};
  uv_stream_t* stream{nullptr};
  uv_async_t stop_async{};
  std::atomic<bool> stopped{false};
  std::thread loop_thread;

  void init_pipe(int fd, const char* which, uv_async_cb on_stop, void* owner) {
    uv_loop_init(&loop);
    const int duped_fd = dup_fd(fd);
    if (duped_fd < 0)
      throw std::runtime_error(std::string{"term_transport: dup ("} + which + ") failed");

    const uv_handle_type guess = uv_guess_handle(duped_fd);
    trace(
        "init_pipe",
        std::string{which} + ": fd=" + std::to_string(fd) + " duped_fd=" + std::to_string(duped_fd) +
            " uv_guess_handle=" + uv_handle_type_name(guess));

    if (guess == UV_TTY) {
      const bool readable = std::strcmp(which, "read") == 0;
      const int r = uv_tty_init(&loop, &handle.tty, duped_fd, readable);
      if (r != 0)
        throw std::runtime_error(std::string{"term_transport: uv_tty_init ("} + which + ") failed: " + uv_strerror(r));
      // uv_tty_init() alone leaves the handle in UV_TTY_MODE_NORMAL, where
      // libuv's Windows console reader waits for a full line and performs
      // its own key/escape translation for interactive use — it never
      // delivers raw bytes as they're written, which is fatal to a framed
      // byte-stream protocol like ours (the handshake just hangs). RAW mode
      // makes libuv treat the console as a plain byte stream instead.
      //
      // Skipped on POSIX: unlike a Windows console handle, a POSIX fd
      // classified UV_TTY here isn't necessarily *this process's own*
      // terminal — on the server side of --transport=term it's the
      // forkpty() master feeding the spawned shell, and tcsetattr() on a
      // pty master mutates the *same shared termios* the slave-side shell
      // runs on (isatty(master_fd) is true, and get/set are forwarded to
      // the paired slave). Calling uv_tty_set_mode(RAW) there clobbers the
      // spawned shell's own terminal settings (echo, canonical mode) out
      // from under it. The legitimate POSIX case — a client wrapping its
      // own real stdio — already puts that terminal in raw mode itself via
      // pty::raw_mode_guard before construction, so this call is redundant
      // there and actively harmful for the pty-master case; only native
      // Windows needs it.
#if defined(_WIN32)
      if (readable) {
        const int mode_r = uv_tty_set_mode(&handle.tty, UV_TTY_MODE_RAW);
        trace("init_pipe", std::string{which} + ": uv_tty_set_mode(RAW) -> " + std::to_string(mode_r));
        if (mode_r != 0)
          throw std::runtime_error(
              std::string{"term_transport: uv_tty_set_mode ("} + which + ") failed: " + uv_strerror(mode_r));
      }
#endif
      stream = reinterpret_cast<uv_stream_t*>(&handle.tty);
    } else {
      uv_pipe_init(&loop, &handle.pipe, 0);
      const int r = uv_pipe_open(&handle.pipe, duped_fd);
      if (r != 0)
        throw std::runtime_error(std::string{"term_transport: uv_pipe_open ("} + which + ") failed: " + uv_strerror(r));
      stream = reinterpret_cast<uv_stream_t*>(&handle.pipe);
    }
    stream->data = owner;
    uv_async_init(&loop, &stop_async, on_stop);
    stop_async.data = owner;
    trace("init_pipe", std::string{which} + ": init complete");
  }

  template <typename PreRun, typename PostRun>
  void start(PreRun&& pre_run, PostRun&& post_run) {
    loop_thread =
        std::thread([this, pre_run = std::forward<PreRun>(pre_run), post_run = std::forward<PostRun>(post_run)] {
          pre_run();
          uv_run(&loop, UV_RUN_DEFAULT);
          post_run();
          uv_loop_close(&loop);
        });
  }

  void stop() {
    if (stopped.exchange(true))
      return;
    if (loop_thread.joinable()) {
      uv_async_send(&stop_async);
      loop_thread.join();
    }
  }
};

// ── Reader: OSC-99/marker scanner + chunk reassembly over one fd ────────────

struct term_reader {
  // See stdio_reader::kIdleFlushMs's doc comment — same rationale applies to
  // a partial match against prefix.
  static constexpr uint64_t kIdleFlushMs = 50;

  term_pipe_thread io;
  std::vector<uint8_t> read_buf = std::vector<uint8_t>(65536);
  uv_timer_t idle_timer{};

  // Which format this side expects to *receive* — the opposite of what its
  // own role sends, per the client/server table on term_role's doc comment.
  // Set once in init(), read-only afterwards.
  std::string_view prefix;
  char terminator{};
  bool osc_format{true};

  // Scanner state — loop thread only.
  bool in_seq{false};
  size_t match_pos{0};
  std::string speculative;
  std::string capture;

  // Chunk reassembly state — loop thread only.
  struct reassembly {
    bool active{false};
    uint32_t total{0};
    uint32_t expected_seq{0};
    bison::buffer buf;
  };
  reassembly assem;

  stdio_passthrough_cb passthrough;
  bison::synchronized<std::queue<bison::buffer>> recv_queue;
  std::atomic<bool> recv_closed{false};

  term_reader() = default;
  ~term_reader() {
    stop();
  }
  term_reader(const term_reader&) = delete;
  term_reader& operator=(const term_reader&) = delete;

  void init(int fd, stdio_passthrough_cb cb, term_role role) {
    passthrough = std::move(cb);
    // See term_role's doc comment: a side receives the format the *other*
    // side's role sends, not its own.
    osc_format = role == term_role::server;
    prefix = osc_format ? kOscPrefix : kMarkerPrefix;
    terminator = osc_format ? kOscTerminator : kMarkerSuffix;
    io.init_pipe(fd, "read", on_stop, this);
    uv_timer_init(&io.loop, &idle_timer);
    idle_timer.data = this;
  }

  void start_loop() {
    io.start(
        [this] { uv_read_start(io.stream, alloc_cb, on_read); },
        [this] {
          recv_closed.store(true);
          recv_queue.notify_all();
        });
  }

  void stop() {
    io.stop();
  }

  bool dequeue_frame(bison::buffer& frame, std::chrono::milliseconds timeout) {
    bool got = false;
    recv_queue.wait_for(timeout, [&](auto& q) {
      if (q.empty() && !recv_closed.load())
        return false;
      if (!q.empty()) {
        frame = std::move(q.front());
        q.pop();
        got = true;
      }
      return true;
    });
    return got;
  }

  // ── Scanner ──────────────────────────────────────────────────────────────

  void feed_byte(uint8_t b) {
    if (in_seq) {
      if (b == static_cast<uint8_t>(terminator)) {
        complete_sequence();
      } else {
        capture.push_back(static_cast<char>(b));
        // The size cap only makes sense for OSC-99 bodies (bounded by
        // kMaxOscSequenceBytes per chunk); marker bodies are un-chunked by
        // design (see kMarkerPrefix's doc comment) and may legitimately be
        // much larger than that.
        if (osc_format && capture.size() > kMaxCaptureBytes) {
          std::cerr << "[term_transport] warning: sequence exceeded max size, discarding\n";
          capture.clear();
          in_seq = false;
        }
      }
      return;
    }

    if (b == static_cast<uint8_t>(prefix[match_pos])) {
      speculative.push_back(static_cast<char>(b));
      ++match_pos;
      if (match_pos == prefix.size()) {
        in_seq = true;
        speculative.clear();
        capture.clear();
        match_pos = 0;
        uv_timer_stop(&idle_timer);
      } else {
        arm_idle_flush();
      }
      return;
    }

    // Mismatch: flush whatever was speculatively held, then reprocess this
    // byte fresh (it may itself restart a match, or belong to some other,
    // unrelated escape sequence that must still reach the real terminal).
    if (!speculative.empty()) {
      passthrough(speculative);
      speculative.clear();
    }
    match_pos = 0;

    if (b == static_cast<uint8_t>(prefix[0])) {
      match_pos = 1;
      speculative.push_back(static_cast<char>(b));
      arm_idle_flush();
    } else {
      uv_timer_stop(&idle_timer);
      const char c = static_cast<char>(b);
      passthrough(std::string_view{&c, 1});
    }
  }

  void arm_idle_flush() {
    uv_timer_start(&idle_timer, on_idle_timeout, kIdleFlushMs, 0);
  }

  static void on_idle_timeout(uv_timer_t* h) {
    auto* self = static_cast<term_reader*>(h->data);
    if (!self->speculative.empty()) {
      self->passthrough(self->speculative);
      self->speculative.clear();
    }
    self->match_pos = 0;
  }

  void complete_sequence() {
    if (osc_format)
      parse_and_complete(capture);
    else
      complete_marker(capture);
    capture.clear();
    in_seq = false;
    match_pos = 0;
    speculative.clear();
  }

  // ── Marker (BISON<base64>) completion: one frame per marker, no chunking —
  //    see kMarkerPrefix's doc comment. ────────────────────────────────────
  void complete_marker(const std::string& body) {
    auto decoded = base64_decode(body);
    if (!decoded) {
      std::cerr << "[term_transport] warning: malformed BISON<...> marker payload ignored\n";
      return;
    }
    recv_queue.withWLock([&](auto& q) { q.push(std::move(*decoded)); });
    recv_queue.notify_one();
  }

  void parse_and_complete(const std::string& body) {
    const auto first_semi = body.find(';');
    if (first_semi == std::string::npos) {
      std::cerr << "[term_transport] warning: malformed OSC-99 sequence ignored (no ';')\n";
      return;
    }
    const auto second_semi = body.find(';', first_semi + 1);
    if (second_semi == std::string::npos) {
      std::cerr << "[term_transport] warning: malformed OSC-99 sequence ignored (no second ';')\n";
      return;
    }
    uint32_t seq = 0;
    uint32_t total = 0;
    if (!parse_uint(std::string_view(body).substr(0, first_semi), seq) ||
        !parse_uint(std::string_view(body).substr(first_semi + 1, second_semi - first_semi - 1), total)) {
      std::cerr << "[term_transport] warning: malformed OSC-99 sequence ignored (bad seq/total)\n";
      return;
    }
    const std::string_view b64(body.data() + second_semi + 1, body.size() - second_semi - 1);
    complete_chunk(seq, total, b64);
  }

  void complete_chunk(uint32_t seq, uint32_t total, std::string_view b64_payload) {
    auto decoded = base64_decode(b64_payload);
    if (!decoded || total == 0) {
      std::cerr << "[term_transport] warning: malformed OSC-99 chunk payload ignored\n";
      return;
    }

    if (seq == 0) {
      if (assem.active && assem.expected_seq != assem.total) {
        std::cerr << "[term_transport] warning: stalled OSC-99 reassembly discarded (" << assem.expected_seq << "/"
                  << assem.total << ")\n";
      }
      assem = reassembly{};
      assem.active = true;
      assem.total = total;
    }

    if (!assem.active || seq != assem.expected_seq || total != assem.total) {
      std::cerr << "[term_transport] warning: out-of-sequence OSC-99 chunk discarded (seq=" << seq
                << ", expected=" << assem.expected_seq << ")\n";
      assem = reassembly{};
      return;
    }

    assem.buf.insert(assem.buf.end(), decoded->begin(), decoded->end());
    ++assem.expected_seq;
    if (assem.expected_seq == assem.total) {
      recv_queue.withWLock([&](auto& q) { q.push(std::move(assem.buf)); });
      recv_queue.notify_one();
      assem = reassembly{};
    }
  }

  // ── Static libuv callbacks ───────────────────────────────────────────────

  static void alloc_cb(uv_handle_t* h, size_t /*suggested*/, uv_buf_t* buf) {
    auto* self = static_cast<term_reader*>(h->data);
    buf->base = reinterpret_cast<char*>(self->read_buf.data());
    buf->len = static_cast<decltype(buf->len)>(self->read_buf.size());
  }

  static void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* /*buf*/) {
    auto* self = static_cast<term_reader*>(stream->data);
    if (nread < 0) {
      trace("on_read", std::string{"error/EOF: "} + uv_strerror(static_cast<int>(nread)));
      self->recv_closed.store(true);
      self->recv_queue.notify_all();
      self->passthrough(std::string_view{}); // signals closure; see stdio_passthrough_cb's doc comment
      uv_read_stop(stream);
      return;
    }
    if (nread == 0)
      return;
    trace("on_read", hex_preview(self->read_buf.data(), static_cast<size_t>(nread)));
    for (ssize_t i = 0; i < nread; ++i)
      self->feed_byte(self->read_buf[static_cast<size_t>(i)]);
  }

  static void on_stop(uv_async_t* h) {
    auto* self = static_cast<term_reader*>(h->data);
    uv_close_if_active(reinterpret_cast<uv_handle_t*>(self->io.stream));
    uv_close_if_active(reinterpret_cast<uv_handle_t*>(&self->io.stop_async));
    uv_close_if_active(reinterpret_cast<uv_handle_t*>(&self->idle_timer));
  }
};

// ── Writer: raw byte sink over one fd ────────────────────────────────────────

struct term_writer {
  term_pipe_thread io;
  uv_async_t send_async{};

  bison::synchronized<std::queue<std::vector<uint8_t>>> send_queue;
  std::atomic<bool> stopped{false};

  term_writer() = default;
  ~term_writer() {
    stop();
  }
  term_writer(const term_writer&) = delete;
  term_writer& operator=(const term_writer&) = delete;

  void init(int fd) {
    io.init_pipe(fd, "write", on_stop, this);
    uv_async_init(&io.loop, &send_async, on_send);
    send_async.data = this;
  }

  void start_loop() {
    io.start([] {}, [] {});
  }

  void stop() {
    if (stopped.exchange(true))
      return;
    io.stop();
  }

  void enqueue_bytes(std::vector<uint8_t> data) {
    if (stopped.load())
      return;
    trace("enqueue_bytes", hex_preview(data.data(), data.size()));
    send_queue.withWLock([&](auto& q) { q.push(std::move(data)); });
    uv_async_send(&send_async);
  }

  static void on_send(uv_async_t* h) {
    auto* self = static_cast<term_writer*>(h->data);
    trace("on_send", "flushing write queue");
    uv_flush_write_queue(self->send_queue, self->io.stream);
  }

  static void on_stop(uv_async_t* h) {
    auto* self = static_cast<term_writer*>(h->data);
    uv_close_if_active(reinterpret_cast<uv_handle_t*>(self->io.stream));
    uv_close_if_active(reinterpret_cast<uv_handle_t*>(&self->send_async));
    uv_close_if_active(reinterpret_cast<uv_handle_t*>(&self->io.stop_async));
  }
};

// ── term_conn_state ──────────────────────────────────────────────────────────

struct term_conn_state {
  term_reader reader;
  term_writer writer;
  std::atomic<bool> closed{false};
  term_role role{term_role::client};

  struct client_handshake_state {
    bool done = false;
    std::string accum;
  };
  bison::synchronized<client_handshake_state> client_handshake;

  bison::synchronized<std::string> server_handshake_accum;

  void start(int read_fd, int write_fd, stdio_passthrough_cb passthrough, term_role r) {
    role = r;
    trace("term_conn_state", "start: read_fd=" + std::to_string(read_fd) + " write_fd=" + std::to_string(write_fd));
    reader.init(read_fd, std::move(passthrough), role);
    writer.init(write_fd);
    reader.start_loop();
    writer.start_loop();
    trace("term_conn_state", "start: loops running");
  }

  /**
   * @brief Encode @p frame for the wire and enqueue it as a single write.
   *        Client role: one or more `\x1b]99;<seq>;<total>;<base64>\x07`
   *        chunks (see kChunkRawBytes) — safe over the client-stdout ->
   *        ConPTY-output direction. Server role: a single un-chunked
   *        `BISON<base64>` marker — required over the server-write ->
   *        ConPTY-input direction (see term_role's doc comment).
   */
  void send_frame(const bison::buffer& frame) {
    if (closed.load())
      throw std::runtime_error("term_transport: send on closed connection");

    if (role == term_role::server) {
      std::string out;
      out.append(kMarkerPrefix);
      out.append(base64_encode(frame));
      out.push_back(kMarkerSuffix);
      writer.enqueue_bytes(std::vector<uint8_t>(out.begin(), out.end()));
      return;
    }

    const size_t total_chunks = frame.empty() ? 1 : (frame.size() + kChunkRawBytes - 1) / kChunkRawBytes;
    std::string out;
    for (size_t i = 0; i < total_chunks; ++i) {
      const size_t offset = i * kChunkRawBytes;
      const size_t len = std::min(kChunkRawBytes, frame.size() - offset);
      const bison::buffer chunk_bytes(
          frame.begin() + static_cast<ptrdiff_t>(offset), frame.begin() + static_cast<ptrdiff_t>(offset + len));
      out.append(kOscPrefix);
      out.append(std::to_string(i));
      out.push_back(';');
      out.append(std::to_string(total_chunks));
      out.push_back(';');
      out.append(base64_encode(chunk_bytes));
      out.push_back(kOscTerminator);
    }
    writer.enqueue_bytes(std::vector<uint8_t>(out.begin(), out.end()));
  }

  void send_raw(std::string_view bytes) {
    if (closed.load())
      return;
    writer.enqueue_bytes(std::vector<uint8_t>(bytes.begin(), bytes.end()));
  }

  void client_passthrough(std::string_view chunk, const stdio_passthrough_cb& real) {
    trace("client_passthrough", hex_preview(reinterpret_cast<const uint8_t*>(chunk.data()), chunk.size()));
    if (chunk.empty()) {
      real(chunk);
      return;
    }
    bool already_done = false;
    bool just_resolved = false;
    std::string leftover;
    client_handshake.withWLock([&](auto& hs) {
      if (hs.done) {
        already_done = true;
        return;
      }
      hs.accum.append(chunk);
      const auto pos = hs.accum.find(kHandshakeOk);
      if (pos != std::string::npos) {
        hs.done = true;
        just_resolved = true;
        leftover = hs.accum.substr(pos + kHandshakeOk.size());
        hs.accum.clear();
      } else if (hs.accum.size() > kHandshakeAccumCap) {
        hs.accum.erase(0, hs.accum.size() - kHandshakeAccumCap);
      }
    });
    if (already_done) {
      real(chunk);
      return;
    }
    if (just_resolved) {
      client_handshake.notify_all();
      if (!leftover.empty())
        real(leftover);
    }
  }

  bool wait_for_client_handshake(std::chrono::milliseconds timeout) {
    return client_handshake.wait_for(timeout, [](const auto& hs) { return hs.done; });
  }

  void watch_for_handshake_start(std::string_view chunk) {
    trace("watch_for_handshake_start", hex_preview(reinterpret_cast<const uint8_t*>(chunk.data()), chunk.size()));
    if (chunk.empty())
      return;
    bool matched = false;
    server_handshake_accum.withWLock([&](std::string& accum) {
      accum.append(chunk);
      const auto pos = accum.find(kHandshakeStartToken);
      if (pos != std::string::npos) {
        matched = true;
        accum.erase(0, pos + kHandshakeStartToken.size());
      }
      if (accum.size() > kHandshakeAccumCap)
        accum.erase(0, accum.size() - kHandshakeAccumCap);
    });
    if (matched) {
      trace("watch_for_handshake_start", "matched START, sending OK");
      send_raw(kHandshakeOk);
    }
  }

  bool receive(bison::buffer& frame, std::chrono::milliseconds timeout) {
    if (closed.load())
      return false;
    return reader.dequeue_frame(frame, timeout);
  }

  void stop() {
    closed.store(true);
    reader.stop();
    writer.stop();
  }
};

// ── term_client_transport ────────────────────────────────────────────────────

term_client_transport::term_client_transport(
    int read_fd,
    int write_fd,
    stdio_passthrough_cb passthrough,
    std::chrono::milliseconds handshake_timeout)
    : state_(std::make_unique<term_conn_state>()), handshake_timeout_(handshake_timeout) {
  auto* state_ptr = state_.get();
  state_->start(
      read_fd, write_fd,
      [state_ptr, real_passthrough = std::move(passthrough)](std::string_view chunk) {
        state_ptr->client_passthrough(chunk, real_passthrough);
      },
      term_role::client);
}

term_client_transport::~term_client_transport() {
  if (state_)
    state_->stop();
}

void term_client_transport::open(bison::dynamic /*params*/) {
  trace("open", "sending START, waiting up to " + std::to_string(handshake_timeout_.count()) + "ms for OK");
  state_->send_raw(kHandshakeStart);
  if (!state_->wait_for_client_handshake(handshake_timeout_)) {
    trace("open", "handshake timed out");
    throw std::runtime_error(
        "term_transport: handshake timed out - sent START BISON/1.0 but got no BISON/1.0 OK "
        "from the peer (is there a bison_server --transport=term running on the other end?)");
  }
}

void term_client_transport::send(bison::buffer frame) {
  state_->send_frame(frame);
}

void term_client_transport::send(std::string_view bytes) {
  state_->send_raw(bytes);
}

bool term_client_transport::receive(bison::buffer& frame, std::chrono::milliseconds timeout) {
  return state_->receive(frame, timeout);
}

void term_client_transport::shutdown() {
  state_->send_raw(kHandshakeStop);
  state_->stop();
}

// ── term_server_connection ───────────────────────────────────────────────────

term_server_connection::term_server_connection(std::shared_ptr<term_conn_state> state, std::function<void()> on_close)
    : state_(std::move(state)), on_close_(std::move(on_close)) {}

term_server_connection::~term_server_connection() {
  close();
}

void term_server_connection::send(bison::buffer frame) {
  state_->send_frame(frame);
}

bool term_server_connection::receive(bison::buffer& frame, std::chrono::milliseconds timeout) {
  if (closed_.load())
    return false;
  return state_->receive(frame, timeout);
}

void term_server_connection::close() {
  if (closed_.exchange(true))
    return;
  if (on_close_)
    on_close_();
}

bool term_server_connection::is_closed() const {
  return closed_.load();
}

// ── term_server_transport ─────────────────────────────────────────────────────

term_server_transport::term_server_transport(int read_fd, int write_fd, stdio_passthrough_cb passthrough)
    : read_fd_(read_fd), write_fd_(write_fd), passthrough_(std::move(passthrough)) {}

term_server_transport::~term_server_transport() {
  if (state_)
    state_->stop();
}

void term_server_transport::start(bison::dynamic /*params*/) {
  stopped_.store(false);
  state_ = std::make_shared<term_conn_state>();
  auto* state_ptr = state_.get();
  state_->start(
      read_fd_, write_fd_,
      [state_ptr, real_passthrough = passthrough_](std::string_view chunk) {
        real_passthrough(chunk);
        state_ptr->watch_for_handshake_start(chunk);
      },
      term_role::server);
}

std::unique_ptr<server_connection_iface> term_server_transport::accept(std::chrono::milliseconds timeout) {
  if (!checked_out_.wait_for(timeout, [this](bool& co) { return !co || stopped_.load(); }))
    return nullptr;
  return checked_out_.withWLock([this](bool& co) -> std::unique_ptr<server_connection_iface> {
    if (stopped_.load() || co)
      return nullptr;
    co = true;
    return std::make_unique<term_server_connection>(state_, [this] {
      checked_out_.withWLock([](bool& c) { c = false; });
      checked_out_.notify_all();
    });
  });
}

void term_server_transport::stop() {
  stopped_.store(true);
  if (state_)
    state_->stop();
  checked_out_.notify_all();
}

} // namespace bdg::bison::rmi::transport

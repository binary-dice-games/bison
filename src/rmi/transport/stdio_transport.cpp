// MIT License © 2025 Binary Dice Games
/**
 * @file stdio_transport.cpp
 * @brief Stdio stream transport implementation for RMI.
 *
 * Tunnels RMI envelope bytes through stdin/stdout using ASCII-safe framing
 * so the transport can operate over interactive channels — ssh, adb shell,
 * a PTY, or a subprocess pipe — without corrupting co-resident terminal text.
 *
 * ### Framing overview
 *
 * Each binary frame is chunked, base64-encoded, and wrapped in one of two
 * carriers:
 *  - **DCS mode**: @c ESC P … ESC \ (preferred; invisible on most terminals).
 *  - **Line mode**: @c @@BISON_RMI@@…\n (fallback for channels that strip DCS).
 *
 * The reader loop extracts either carrier from the raw byte stream and ignores
 * everything else, so normal application text on stdout is transparent.
 *
 * ### Thread model
 *
 * A single reader thread per endpoint parses the input stream and pushes
 * complete reassembled frames onto @c shared_state::inbox.  The send path
 * writes to stdout under @c shared_state::write_mtx so frames are not
 * interleaved.  All remaining shared mutable state uses @c read_mtx /
 * @c read_cv or atomics; see @c shared_state for the locking contract.
 *
 * ### Startup handshake
 *
 * The server emits a HELLO frame immediately after @c start().  In
 * @c auto_detect mode it emits HELLO in both DCS and line format so the
 * client can choose the mode that survives the channel.  The client blocks
 * in @c open() until a HELLO arrives or @c handshake_timeout elapses.
 */
#include "src/rmi/transport/stdio_transport.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
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
#include <utility>
#include <vector>

#if defined(__linux__)
#include <termios.h>
#include <unistd.h>
#endif

namespace bdg::bison::rmi::transport {

namespace {

// ── Framing constants ──────────────────────────────────────────────────────

/** @brief Framing mode chosen for the session. */
enum class framing_mode {
  auto_detect, ///< Choose mode from the first successful HELLO exchange.
  dcs,         ///< DCS control-string carrier: ESC P … ESC \.
  line,        ///< Line-prefix carrier: @@BISON_RMI@@…\n.
};

constexpr char        kEsc                  = '\x1b';
constexpr const char* kDcsStart             = "\x1bP";
constexpr const char* kDcsEnd               = "\x1b\\";
constexpr const char* kLinePrefix           = "@@BISON_RMI@@";
constexpr const char* kProtocolVersionToken = "BISON_RMI/1";
constexpr const char* kTypeData             = "DATA";
constexpr const char* kTypeHello            = "HELLO";
constexpr const char* kTypeEnd              = "END";

// ── Internal types ─────────────────────────────────────────────────────────

/** @brief Reassembly record for one in-flight chunked message. */
struct partial_message {
  uint32_t total = 0;                              ///< Expected chunk count.
  std::vector<std::optional<bison::buffer>> parts; ///< Per-seq chunk buffers.
  size_t collected = 0;                            ///< Bytes accumulated so far.
  std::chrono::steady_clock::time_point first_seen;///< Timestamp for expiry.
};

/**
 * @brief All mutable state shared between the reader thread and the public API.
 *
 * Owned by @c shared_ptr so both the reader thread and the public transport
 * class can hold a reference without complex lifetime coordination.
 *
 * ### Locking rules
 *
 *  - @c inbox and @c pending are protected by @c read_mtx; transitions that
 *    unblock a waiting receiver are signalled via @c read_cv.
 *  - stdout writes are serialised by @c write_mtx.
 *  - @c closed, @c stop_requested, @c hello_seen, and @c negotiated_mode are
 *    atomics and may be read or written without holding any mutex.
 *  - All other fields are written once before the reader thread starts and are
 *    treated as read-only thereafter.
 */
struct shared_state {
  // ── I/O streams ───────────────────────────────────────────────────────────
  std::istream* in  = nullptr;
  std::ostream* out = nullptr;

#if defined(__linux__)
  bool use_fds  = false;
  int  read_fd  = -1;
  int  write_fd = -1;
#endif

  // ── Inbox (protected by read_mtx) ─────────────────────────────────────────
  std::mutex              read_mtx;
  std::condition_variable read_cv;
  std::queue<bison::buffer>                     inbox;
  std::unordered_map<uint64_t, partial_message> pending;

  // ── Write serialisation ───────────────────────────────────────────────────
  std::mutex write_mtx;

  // ── Atomics ───────────────────────────────────────────────────────────────
  std::atomic<bool> closed{false};
  std::atomic<bool> stop_requested{false};
  std::atomic<bool> hello_seen{false};

  /**
   * Negotiated framing mode, stored as @c int for lock-free atomic access.
   * The reader thread (setter in process_body) and the write thread (getter
   * in effective_mode) both access this field concurrently.  Cast to/from
   * @c framing_mode at each call site.
   */
  std::atomic<int> negotiated_mode{static_cast<int>(framing_mode::auto_detect)};

  // ── Configuration (write-once before reader thread starts) ────────────────
  framing_mode              send_mode            = framing_mode::dcs;
  std::chrono::milliseconds handshake_timeout{5000};
  std::chrono::milliseconds reassembly_timeout{5000};
  size_t                    max_frame_bytes      = 8U * 1024U * 1024U;
  size_t                    max_chunk_bytes      = 1536U;
  bool                      mirror_plaintext_to_stderr = false;

  std::atomic<uint64_t> next_message_id{1};
};

// ── Low-level I/O ─────────────────────────────────────────────────────────

/**
 * @brief Read one byte from the transport's input source.
 *
 * On Linux, uses the raw file descriptor when @c state.use_fds is set,
 * retrying on @c EINTR.
 *
 * @return Byte value in [0, 255], or @c EOF on end-of-stream or error.
 */
int read_next_byte(shared_state& state) {
#if defined(__linux__)
  if (state.use_fds) {
    while (true) {
      uint8_t c = 0;
      const auto n = ::read(state.read_fd, &c, 1);
      if (n == 1)
        return static_cast<int>(c);
      if (n == 0)
        return EOF;
      if (errno == EINTR)
        continue;
      return EOF;
    }
  }
#endif
  if (!state.in)
    return EOF;
  return state.in->get();
}

/**
 * @brief Write @p text to the transport's output in full.
 *
 * On Linux, uses the raw file descriptor when @c state.use_fds is set,
 * retrying on @c EINTR.
 *
 * @return @c true on success; @c false if the stream or descriptor failed.
 */
bool write_all(shared_state& state, const std::string& text) {
#if defined(__linux__)
  if (state.use_fds) {
    size_t sent = 0;
    while (sent < text.size()) {
      const auto n =
          ::write(state.write_fd, text.data() + sent, text.size() - sent);
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
#endif
  if (!state.out)
    return false;
  (*state.out) << text;
  state.out->flush();
  return static_cast<bool>(*state.out);
}

// ── Base-64 codec ──────────────────────────────────────────────────────────

/**
 * @brief Decode one RFC 4648 base64 character to its 6-bit value.
 * @return Value in [0, 63], or -1 for an invalid character.
 */
int from_base64_char(char c) {
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

/**
 * @brief Encode @p size bytes at @p data as RFC 4648 base64.
 *
 * Output length is always a multiple of 4; padding @c = characters are
 * appended as required.
 */
std::string base64_encode(const uint8_t* data, size_t size) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  std::string out;
  out.reserve(((size + 2) / 3) * 4);

  for (size_t i = 0; i < size; i += 3) {
    const uint32_t b0 = data[i];
    const uint32_t b1 = (i + 1 < size) ? data[i + 1] : 0u;
    const uint32_t b2 = (i + 2 < size) ? data[i + 2] : 0u;

    const uint32_t n = (b0 << 16) | (b1 << 8) | b2;
    out.push_back(kAlphabet[(n >> 18) & 0x3f]);
    out.push_back(kAlphabet[(n >> 12) & 0x3f]);
    out.push_back((i + 1 < size) ? kAlphabet[(n >> 6) & 0x3f] : '=');
    out.push_back((i + 2 < size) ? kAlphabet[n & 0x3f]        : '=');
  }

  return out;
}

/**
 * @brief Decode RFC 4648 base64 @p input into @p out.
 *
 * @p input length must be a multiple of 4.
 * @return @c true on success; @c false on malformed input.
 */
bool base64_decode(const std::string& input, bison::buffer& out) {
  out.clear();
  if (input.empty())
    return true;
  if ((input.size() % 4) != 0)
    return false;

  out.reserve((input.size() / 4) * 3);

  for (size_t i = 0; i < input.size(); i += 4) {
    const char c0 = input[i + 0];
    const char c1 = input[i + 1];
    const char c2 = input[i + 2];
    const char c3 = input[i + 3];

    const int v0 = from_base64_char(c0);
    const int v1 = from_base64_char(c1);
    if (v0 < 0 || v1 < 0)
      return false;

    const bool pad2 = (c2 == '=');
    const bool pad3 = (c3 == '=');
    const int  v2   = pad2 ? 0 : from_base64_char(c2);
    const int  v3   = pad3 ? 0 : from_base64_char(c3);
    if ((!pad2 && v2 < 0) || (!pad3 && v3 < 0))
      return false;

    const uint32_t n = (static_cast<uint32_t>(v0) << 18) |
        (static_cast<uint32_t>(v1) << 12) | (static_cast<uint32_t>(v2) << 6) |
        static_cast<uint32_t>(v3);

    out.push_back(static_cast<uint8_t>((n >> 16) & 0xff));
    if (!pad2)
      out.push_back(static_cast<uint8_t>((n >> 8) & 0xff));
    if (!pad3)
      out.push_back(static_cast<uint8_t>(n & 0xff));
  }

  return true;
}

// ── Text helpers ──────────────────────────────────────────────────────────

/** @brief Return @c true if @p value begins with @p prefix. */
bool starts_with(const std::string& value, const std::string& prefix) {
  return value.size() >= prefix.size() &&
      value.compare(0, prefix.size(), prefix) == 0;
}

/** @brief Return a copy of @p input with leading and trailing whitespace
 *         stripped. */
std::string trim_copy(const std::string& input) {
  size_t begin = 0;
  while (begin < input.size() &&
         std::isspace(static_cast<unsigned char>(input[begin])))
    ++begin;

  size_t end = input.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(input[end - 1])))
    --end;
  return input.substr(begin, end - begin);
}

/**
 * @brief Return @c true if @p text could still grow into the line-prefix
 *        marker @c @@BISON_RMI@@.
 *
 * Used to suppress plaintext mirroring of a partial line until it is clear
 * the line is not a protocol frame.
 */
bool could_be_line_frame_prefix(const std::string& text) {
  const std::string prefix{kLinePrefix};
  if (text.size() > prefix.size())
    return false;
  return std::equal(text.begin(), text.end(), prefix.begin());
}

// ── Parameter parsing ──────────────────────────────────────────────────────

/**
 * @brief Parse a decimal string to @c uint64_t.
 * @return Parsed value, or @c nullopt on any error or trailing characters.
 */
std::optional<uint64_t> parse_u64(const std::string& value) {
  try {
    size_t pos = 0;
    const auto parsed = std::stoull(value, &pos, 10);
    if (pos != value.size())
      return std::nullopt;
    return parsed;
  } catch (...) {
    return std::nullopt;
  }
}

/**
 * @brief Parse a decimal string to @c uint32_t.
 * @return Parsed value, or @c nullopt on error or overflow.
 */
std::optional<uint32_t> parse_u32(const std::string& value) {
  const auto n = parse_u64(value);
  if (!n)
    return std::nullopt;
  if (*n > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()))
    return std::nullopt;
  return static_cast<uint32_t>(*n);
}

/**
 * @brief Map a string parameter to a @c framing_mode.
 *
 * Recognises @c "auto", @c "dcs", and @c "line".  Returns @p fallback for
 * any unrecognised value.
 */
framing_mode parse_mode_param(const std::string& value, framing_mode fallback) {
  const auto mode = trim_copy(value);
  if (mode == "auto")
    return framing_mode::auto_detect;
  if (mode == "dcs")
    return framing_mode::dcs;
  if (mode == "line")
    return framing_mode::line;
  return fallback;
}

/**
 * @brief Parse the semicolon-delimited control body into a key-value map.
 *
 * The first token must be the protocol version string @c BISON_RMI/1.
 * Subsequent tokens are @c key=value pairs.
 *
 * @return Populated map on success; empty map if the version token is wrong
 *         or absent.
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
      if (token != kProtocolVersionToken)
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

/**
 * @brief Resolve the effective framing mode for the next send.
 *
 * When @c send_mode is @c auto_detect, returns the negotiated mode that was
 * fixed during the HELLO exchange, falling back to DCS if negotiation has
 * not yet completed.
 *
 * @note @c negotiated_mode is atomic, so this read is safe from the write
 *       thread without holding any additional lock.
 */
framing_mode effective_mode(const shared_state& state) {
  if (state.send_mode == framing_mode::auto_detect) {
    const auto neg = static_cast<framing_mode>(state.negotiated_mode.load());
    if (neg != framing_mode::auto_detect)
      return neg;
    return framing_mode::dcs;
  }
  return state.send_mode;
}

/**
 * @brief Push a complete frame onto the inbox and wake blocked receivers.
 *
 * Acquires @c read_mtx for the push, then signals @em after releasing the
 * lock so waiting threads do not contend immediately on wake-up.
 */
void push_inbound(shared_state& state, bison::buffer frame) {
  {
    std::lock_guard<std::mutex> lk(state.read_mtx);
    state.inbox.push(std::move(frame));
  }
  state.read_cv.notify_all();
}

/**
 * @brief Mark the state as closed and wake all condition-variable waiters.
 *
 * Called on EOF, on a received END frame, and from @c stop() / @c shutdown().
 */
void close_and_notify(shared_state& state) {
  state.closed.store(true);
  state.read_cv.notify_all();
}

/** @brief Clear the inbox queue and pending reassembly state.
 *
 *  Must only be called before the reader thread starts, or after it has
 *  stopped, to avoid concurrent access. */
void reset_receive_state(shared_state& state) {
  std::lock_guard<std::mutex> lk(state.read_mtx);
  state.inbox   = std::queue<bison::buffer>{};
  state.pending.clear();
}

/**
 * @brief Drop any partial messages older than @c state.reassembly_timeout.
 *
 * Called from @c handle_data_frame while @c read_mtx is held.
 */
void maybe_cleanup_expired(shared_state& state) {
  const auto now = std::chrono::steady_clock::now();
  for (auto it = state.pending.begin(); it != state.pending.end();) {
    if (now - it->second.first_seen > state.reassembly_timeout)
      it = state.pending.erase(it);
    else
      ++it;
  }
}

// ── Frame dispatch (reader thread) ────────────────────────────────────────

/**
 * @brief Accumulate a DATA chunk and, when all chunks arrive, deliver the
 *        complete frame to the inbox.
 *
 * Chunks are keyed by message id in @c state.pending.  When every chunk
 * [0, total-1] has arrived they are concatenated and handed to
 * @c push_inbound.  The lock is released before the push so waiting receivers
 * do not contend for @c read_mtx during notification.
 *
 * @param fields  Parsed key-value map from the control-string body.
 */
void handle_data_frame(
    shared_state& state,
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

  // !id / !seq / !total test whether parsing produced a value (not nullopt).
  // seq is 0-based, so value 0 is valid and does not trigger !seq.
  if (!id || !seq || !total || *total == 0 || *seq >= *total)
    return;

  bison::buffer chunk;
  if (!base64_decode(it_b64->second, chunk))
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
      // Inconsistent total across chunks — discard the whole message.
      state.pending.erase(*id);
      return;
    }

    auto& slot = pm.parts[*seq];
    if (slot.has_value())
      return; // Duplicate chunk — ignore.

    pm.collected += chunk.size();
    if (pm.collected > state.max_frame_bytes) {
      state.pending.erase(*id);
      return;
    }

    slot = std::move(chunk);

    for (const auto& part : pm.parts) {
      if (!part.has_value())
        return; // Still waiting for more chunks.
    }

    // All chunks present — assemble the complete frame.
    assembled.reserve(pm.collected);
    for (auto& part : pm.parts)
      assembled.insert(assembled.end(), part->begin(), part->end());
    state.pending.erase(*id);
  } // read_mtx released before push_inbound to minimise contention on notify

  push_inbound(state, std::move(assembled));
}

/**
 * @brief Dispatch a decoded control-string body to the appropriate handler.
 *
 * Recognised types: HELLO, END, DATA.  Unknown types are silently ignored.
 *
 * @param mode  Framing carrier in which this body arrived (DCS or line).
 *              Used to fix the session mode on the first HELLO.
 */
void process_body(
    shared_state& state,
    const std::string& body,
    framing_mode mode) {
  const auto fields = parse_fields(body);
  if (fields.empty())
    return;

  const auto it_type = fields.find("type");
  if (it_type == fields.end())
    return;

  if (it_type->second == kTypeHello) {
    // Fix the session mode on the first HELLO; subsequent HELLOs are ignored.
    const int auto_val = static_cast<int>(framing_mode::auto_detect);
    if (state.negotiated_mode.load() == auto_val)
      state.negotiated_mode.store(static_cast<int>(mode));
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

/**
 * @brief Process one newline-terminated line from the input stream.
 *
 * Lines that carry the @c @@BISON_RMI@@ prefix are decoded as protocol
 * frames.  All other lines are silently discarded (mirroring is handled
 * separately by @c mirror_plaintext_fragment).
 */
void process_line(shared_state& state, const std::string& line) {
  auto trimmed = trim_copy(line);
  if (trimmed.empty())
    return;

  if (starts_with(trimmed, kLinePrefix)) {
    process_body(
        state,
        trimmed.substr(std::string{kLinePrefix}.size()),
        framing_mode::line);
  }
}

// ── Plaintext mirroring ────────────────────────────────────────────────────

/**
 * @brief Optionally mirror unmirrored bytes from @p line_buf to stderr.
 *
 * Only the bytes beyond @p mirrored_bytes (already mirrored) are written,
 * so incremental calls on a growing line buffer do not duplicate output.
 * Lines that begin with a potential protocol prefix are suppressed until the
 * prefix has been fully seen — this avoids mirroring the start of a frame.
 *
 * @param mirrored_bytes  In/out: count of bytes already mirrored from
 *                        @p line_buf.  Updated on each call.
 */
void mirror_plaintext_fragment(
    shared_state& state,
    const std::string& line_buf,
    size_t& mirrored_bytes) {
  if (!state.mirror_plaintext_to_stderr || line_buf.empty())
    return;

  if (could_be_line_frame_prefix(line_buf))
    return;

  if (mirrored_bytes >= line_buf.size())
    return;

  std::cerr.write(
      line_buf.data() + static_cast<std::ptrdiff_t>(mirrored_bytes),
      static_cast<std::streamsize>(line_buf.size() - mirrored_bytes));
  std::cerr.flush();
  mirrored_bytes = line_buf.size();
}

// ── Reader thread ─────────────────────────────────────────────────────────

/**
 * @brief Background thread: parse the input stream until closed or EOF.
 *
 * Runs a manual state machine that recognises two carriers concurrently:
 *  - DCS blocks started by @c ESC P and closed by @c ESC \.
 *  - Line frames prefixed by @c @@BISON_RMI@@.
 *
 * Bytes outside either carrier are treated as plaintext and optionally
 * mirrored to stderr.  The function exits on EOF or @c stop_requested.
 */
void reader_loop(std::shared_ptr<shared_state> state) {
  if (!state)
    return;

  bool has_input_source = (state->in != nullptr);
#if defined(__linux__)
  has_input_source = has_input_source || state->use_fds;
#endif
  if (!has_input_source)
    return;

  bool        in_dcs      = false;
  bool        saw_esc     = false;
  bool        dcs_saw_esc = false;
  std::string line_buf;
  std::string dcs_buf;
  size_t      mirrored_bytes = 0;

  while (!state->stop_requested.load()) {
    const int ci = read_next_byte(*state);
    if (ci == EOF) {
      mirror_plaintext_fragment(*state, line_buf, mirrored_bytes);
      close_and_notify(*state);
      return;
    }

    const char c = static_cast<char>(ci);

    // ── DCS body accumulator ──────────────────────────────────────────────
    if (in_dcs) {
      if (dcs_saw_esc) {
        if (c == '\\') {
          // ESC \ is the String Terminator — dispatch the completed DCS body.
          process_body(*state, dcs_buf, framing_mode::dcs);
          dcs_buf.clear();
          dcs_saw_esc = false;
          in_dcs      = false;
          continue;
        }
        // ESC followed by something other than \ — not ST; keep the ESC.
        dcs_buf.push_back(kEsc);
        dcs_saw_esc = false;
      }

      if (c == kEsc) {
        dcs_saw_esc = true;
        continue;
      }

      dcs_buf.push_back(c);
      continue;
    }

    // ── ESC lookahead (outside DCS) ───────────────────────────────────────
    if (saw_esc) {
      saw_esc = false;
      if (c == 'P') {
        // ESC P opens a DCS block.
        in_dcs = true;
        dcs_buf.clear();
        continue;
      }
      // Not a recognised escape — treat the ESC as a literal plaintext byte.
      line_buf.push_back(kEsc);
    }

    if (c == kEsc) {
      saw_esc = true;
      continue;
    }

    // ── Line accumulator ──────────────────────────────────────────────────
    line_buf.push_back(c);
    if (c == '\n') {
      mirror_plaintext_fragment(*state, line_buf, mirrored_bytes);
      process_line(*state, line_buf);
      line_buf.clear();
      mirrored_bytes = 0;
      continue;
    }

    mirror_plaintext_fragment(*state, line_buf, mirrored_bytes);
  }

  mirror_plaintext_fragment(*state, line_buf, mirrored_bytes);
  close_and_notify(*state);
}

// ── Frame emission ────────────────────────────────────────────────────────

/**
 * @brief Write one complete control frame to the output under @c write_mtx.
 *
 * In DCS mode: @c ESC P <body> ESC \.
 * In line mode: @c @@BISON_RMI@@ <body> \n.
 *
 * @throws std::runtime_error if the write fails.
 */
void emit_control(
    shared_state& state,
    framing_mode mode,
    const std::string& body) {
  std::lock_guard<std::mutex> lk(state.write_mtx);

  std::string frame;
  if (mode == framing_mode::line)
    frame = std::string{kLinePrefix} + body + "\n";
  else
    frame = std::string{kDcsStart} + body + kDcsEnd;

  if (!write_all(state, frame))
    throw std::runtime_error("stdio transport write failed");
}

/**
 * @brief Split @p frame into chunks, base64-encode them, and emit DATA frames.
 *
 * Each chunk is sent as a separate control frame with the same message id
 * but a unique, zero-based seq value.  An empty @p frame is sent as a single
 * DATA message with @c total=1 and an empty @c b64 field so the receiver can
 * deliver a zero-byte frame rather than silently dropping the message.
 *
 * @throws std::runtime_error if the frame exceeds @c max_frame_bytes or a
 *         write fails.
 */
void emit_data(shared_state& state, const bison::buffer& frame) {
  if (frame.size() > state.max_frame_bytes)
    throw std::runtime_error("stdio transport frame exceeds max_frame_bytes");

  const size_t   chunk_size = std::max<size_t>(1U, state.max_chunk_bytes);
  const uint64_t msg_id     = state.next_message_id.fetch_add(1);
  const auto     mode       = effective_mode(state);

  // Empty frames get a single chunk with b64="" so the receiver delivers
  // a zero-byte payload rather than dropping the message.
  if (frame.empty()) {
    const std::string body = std::string{kProtocolVersionToken} +
        ";type=DATA;id=" + std::to_string(msg_id) + ";seq=0;total=1;b64=";
    emit_control(state, mode, body);
    return;
  }

  const uint32_t total = static_cast<uint32_t>(
      (frame.size() + chunk_size - 1) / chunk_size);

  for (uint32_t seq = 0; seq < total; ++seq) {
    const size_t offset = static_cast<size_t>(seq) * chunk_size;
    const size_t len    = std::min(chunk_size, frame.size() - offset);
    const auto   b64    = base64_encode(frame.data() + offset, len);

    const std::string body = std::string{kProtocolVersionToken} +
        ";type=DATA;id=" + std::to_string(msg_id) +
        ";seq="          + std::to_string(seq)    +
        ";total="        + std::to_string(total)  +
        ";b64="          + b64;

    emit_control(state, mode, body);
  }
}

// ── Configuration ─────────────────────────────────────────────────────────

/**
 * @brief Apply transport parameters from a @c bison::dynamic object.
 *
 * Each key is recognised under both its plain name and its double-underscore
 * internal alias (e.g., @c mode and @c __mode).  Unrecognised keys are
 * silently ignored.
 *
 * Recognised keys:
 *  - @c mode / @c __mode — @c "auto" | @c "dcs" | @c "line"
 *  - @c handshake_timeout_ms — int32, milliseconds
 *  - @c reassembly_timeout_ms — int32, milliseconds
 *  - @c max_frame_bytes — int32, minimum 1024
 *  - @c max_chunk_b64_bytes — int32, minimum 64
 *  - @c mirror_plaintext_to_stderr — bool
 */
void apply_common_params(shared_state& state, const bison::dynamic& params) {
  if (const auto* mode_f = params.findField("mode"_key);
      mode_f != nullptr && mode_f->is<std::string>()) {
    state.send_mode =
        parse_mode_param(mode_f->as<std::string>(), state.send_mode);
  }
  if (const auto* mode_f = params.findField("__mode"_key);
      mode_f != nullptr && mode_f->is<std::string>()) {
    state.send_mode =
        parse_mode_param(mode_f->as<std::string>(), state.send_mode);
  }

  if (const auto* f = params.findField("handshake_timeout_ms"_key);
      f != nullptr && f->is<int32_t>()) {
    state.handshake_timeout =
        std::chrono::milliseconds{std::max(0, f->as<int32_t>())};
  }
  if (const auto* f = params.findField("__handshake_timeout_ms"_key);
      f != nullptr && f->is<int32_t>()) {
    state.handshake_timeout =
        std::chrono::milliseconds{std::max(0, f->as<int32_t>())};
  }

  if (const auto* f = params.findField("reassembly_timeout_ms"_key);
      f != nullptr && f->is<int32_t>()) {
    state.reassembly_timeout =
        std::chrono::milliseconds{std::max(0, f->as<int32_t>())};
  }
  if (const auto* f = params.findField("__reassembly_timeout_ms"_key);
      f != nullptr && f->is<int32_t>()) {
    state.reassembly_timeout =
        std::chrono::milliseconds{std::max(0, f->as<int32_t>())};
  }

  if (const auto* f = params.findField("max_frame_bytes"_key);
      f != nullptr && f->is<int32_t>()) {
    state.max_frame_bytes =
        static_cast<size_t>(std::max(1024, f->as<int32_t>()));
  }
  if (const auto* f = params.findField("__max_frame_bytes"_key);
      f != nullptr && f->is<int32_t>()) {
    state.max_frame_bytes =
        static_cast<size_t>(std::max(1024, f->as<int32_t>()));
  }

  if (const auto* f = params.findField("max_chunk_b64_bytes"_key);
      f != nullptr && f->is<int32_t>()) {
    state.max_chunk_bytes = static_cast<size_t>(std::max(64, f->as<int32_t>()));
  }
  if (const auto* f = params.findField("__max_chunk_b64_bytes"_key);
      f != nullptr && f->is<int32_t>()) {
    state.max_chunk_bytes = static_cast<size_t>(std::max(64, f->as<int32_t>()));
  }

  if (const auto* f = params.findField("mirror_plaintext_to_stderr"_key);
      f != nullptr && f->is<bool>()) {
    state.mirror_plaintext_to_stderr = f->as<bool>();
  }
  if (const auto* f = params.findField("__mirror_plaintext_to_stderr"_key);
      f != nullptr && f->is<bool>()) {
    state.mirror_plaintext_to_stderr = f->as<bool>();
  }
}

/**
 * @brief Block until the peer's HELLO frame has been received.
 *
 * If @c send_mode is already @c line (channel is known to be line-oriented)
 * the negotiated mode is pre-set without waiting for a HELLO frame, avoiding
 * a DCS-mode handshake attempt that the channel might not survive.
 *
 * @throws std::runtime_error on timeout or if the channel closes first.
 */
void wait_for_hello(shared_state& state) {
  if (state.send_mode == framing_mode::line)
    state.negotiated_mode.store(static_cast<int>(framing_mode::line));

  std::unique_lock<std::mutex> lk(state.read_mtx);
  const bool ok = state.read_cv.wait_for(lk, state.handshake_timeout, [&state] {
    return state.hello_seen.load() || state.closed.load();
  });

  if (!ok || !state.hello_seen.load())
    throw std::runtime_error("stdio_client_transport::open handshake timeout");
}

} // namespace

// ── impl structs ──────────────────────────────────────────────────────────

struct stdio_client_transport::impl {
  explicit impl(std::istream& in, std::ostream& out)
      : state(std::make_shared<shared_state>()) {
    state->in  = &in;
    state->out = &out;
  }

#if defined(__linux__)
  impl(int read_fd, int write_fd) : state(std::make_shared<shared_state>()) {
    state->use_fds   = true;
    state->read_fd   = read_fd;
    state->write_fd  = write_fd;
  }
#endif

  std::shared_ptr<shared_state> state;
  std::thread reader;
  bool        opened           = false;
  bool        reader_detached  = false;
};

struct stdio_server_connection::impl {
  explicit impl(std::shared_ptr<shared_state> s) : state(std::move(s)) {}

  std::shared_ptr<shared_state> state;
};

struct stdio_server_transport::impl {
  explicit impl(std::istream& in, std::ostream& out)
      : state(std::make_shared<shared_state>()) {
    state->in  = &in;
    state->out = &out;
  }

  std::shared_ptr<shared_state> state;
  std::thread          reader;
  std::atomic<bool>    running{false};
  std::atomic<bool>    accepted{false};
  bool                 reader_detached = false;
#if defined(__linux__)
  bool    tty_mode_active = false;
  termios saved_tty_mode{};
#endif
};

// ── Linux TTY helpers ─────────────────────────────────────────────────────

#if defined(__linux__)
/**
 * @brief Switch the controlling terminal to raw, no-echo mode.
 *
 * Only acts when the transport is reading from @c std::cin and that
 * file descriptor is a TTY.  Saves the original attributes in
 * @p saved_tty_mode for restoration via @c restore_server_tty_mode.
 *
 * @return @c true if the mode was changed, @c false if no-op.
 */
bool enable_server_tty_raw_mode(
    const shared_state& state,
    bool& tty_mode_active,
    termios& saved_tty_mode) {
  if (state.in != &std::cin || !::isatty(STDIN_FILENO))
    return false;

  termios tty{};
  if (::tcgetattr(STDIN_FILENO, &tty) != 0)
    return false;

  saved_tty_mode = tty;

  tty.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
  tty.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL));
  tty.c_cc[VMIN]  = 1;
  tty.c_cc[VTIME] = 0;

  if (::tcsetattr(STDIN_FILENO, TCSANOW, &tty) != 0)
    return false;

  tty_mode_active = true;
  return true;
}

/** @brief Restore terminal attributes saved by @c enable_server_tty_raw_mode. */
void restore_server_tty_mode(
    bool& tty_mode_active,
    const termios& saved_tty_mode) {
  if (!tty_mode_active)
    return;
  (void)::tcsetattr(STDIN_FILENO, TCSANOW, &saved_tty_mode);
  tty_mode_active = false;
}
#endif

// ══════════════════════════════════════════════════════════════════════════════
// stdio_client_transport
// ══════════════════════════════════════════════════════════════════════════════

stdio_client_transport::stdio_client_transport()
    : impl_(std::make_unique<impl>(std::cin, std::cout)) {}

stdio_client_transport::stdio_client_transport(
    std::istream& in,
    std::ostream& out)
    : impl_(std::make_unique<impl>(in, out)) {}

#if defined(__linux__)
stdio_client_transport::stdio_client_transport(int read_fd, int write_fd)
    : impl_(std::make_unique<impl>(read_fd, write_fd)) {}
#endif

stdio_client_transport::~stdio_client_transport() {
  shutdown();
}

stdio_client_transport::stdio_client_transport(
    stdio_client_transport&&) noexcept = default;
stdio_client_transport& stdio_client_transport::operator=(
    stdio_client_transport&&) noexcept = default;

void stdio_client_transport::open(bison::dynamic params) {
  if (!impl_ || !impl_->state)
    throw std::runtime_error("stdio_client_transport::open invalid state");
  if (impl_->reader_detached)
    throw std::runtime_error(
        "stdio_client_transport::open cannot reopen after detached shutdown");

  impl_->state->stop_requested.store(false);
  impl_->state->closed.store(false);
  impl_->state->hello_seen.store(false);
  impl_->state->negotiated_mode.store(
      static_cast<int>(framing_mode::auto_detect));
  reset_receive_state(*impl_->state);
  apply_common_params(*impl_->state, params);

  if (!impl_->reader.joinable())
    impl_->reader = std::thread(reader_loop, impl_->state);

  wait_for_hello(*impl_->state);
  impl_->opened = true;
}

void stdio_client_transport::send(bison::buffer frame) {
  if (!impl_ || !impl_->state || !impl_->opened)
    throw std::runtime_error("stdio_client_transport::send not open");
  if (impl_->state->closed.load())
    throw std::runtime_error("stdio_client_transport::send closed");
  emit_data(*impl_->state, frame);
}

bool stdio_client_transport::receive(
    bison::buffer& frame,
    std::chrono::milliseconds timeout) {
  if (!impl_ || !impl_->state || !impl_->opened)
    return false;

  std::unique_lock<std::mutex> lk(impl_->state->read_mtx);
  if (!impl_->state->read_cv.wait_for(lk, timeout, [this] {
        return !impl_->state->inbox.empty() || impl_->state->closed.load();
      }))
    return false;

  if (impl_->state->inbox.empty())
    return false;

  frame = std::move(impl_->state->inbox.front());
  impl_->state->inbox.pop();
  return true;
}

void stdio_client_transport::shutdown() {
  if (!impl_ || !impl_->state)
    return;

  // Send END before marking closed so the write path is still available.
  if (impl_->opened && !impl_->state->closed.load()) {
    try {
      const std::string end_body =
          std::string{kProtocolVersionToken} + ";type=END";
      emit_control(*impl_->state, effective_mode(*impl_->state), end_body);
    } catch (...) {
    }
  }

  impl_->state->stop_requested.store(true);
  impl_->state->closed.store(true);
  impl_->state->read_cv.notify_all();

  if (impl_->reader.joinable()) {
    // The reader may be blocked in a syscall; detach to avoid deadlock.
    // The transport is considered single-use after this point.
    impl_->reader.detach();
    impl_->reader_detached = true;
  }

  impl_->opened = false;
}

// ══════════════════════════════════════════════════════════════════════════════
// stdio_server_connection
// ══════════════════════════════════════════════════════════════════════════════

stdio_server_connection::stdio_server_connection() = default;

stdio_server_connection::stdio_server_connection(std::unique_ptr<impl> impl)
    : impl_(std::move(impl)) {}

stdio_server_connection::~stdio_server_connection() {
  close();
}

stdio_server_connection::stdio_server_connection(
    stdio_server_connection&&) noexcept = default;
stdio_server_connection& stdio_server_connection::operator=(
    stdio_server_connection&&) noexcept = default;

void stdio_server_connection::send(bison::buffer frame) {
  if (!impl_ || !impl_->state || impl_->state->closed.load())
    throw std::runtime_error("stdio_server_connection::send closed");
  emit_data(*impl_->state, frame);
}

bool stdio_server_connection::receive(
    bison::buffer& frame,
    std::chrono::milliseconds timeout) {
  if (!impl_ || !impl_->state || impl_->state->closed.load())
    return false;

  std::unique_lock<std::mutex> lk(impl_->state->read_mtx);
  if (!impl_->state->read_cv.wait_for(lk, timeout, [this] {
        return !impl_->state->inbox.empty() || impl_->state->closed.load();
      }))
    return false;

  if (impl_->state->inbox.empty())
    return false;

  frame = std::move(impl_->state->inbox.front());
  impl_->state->inbox.pop();
  return true;
}

void stdio_server_connection::close() {
  if (!impl_ || !impl_->state)
    return;

  impl_->state->stop_requested.store(true);
  impl_->state->closed.store(true);
  impl_->state->read_cv.notify_all();
}

bool stdio_server_connection::is_closed() const {
  return !impl_ || !impl_->state || impl_->state->closed.load();
}

// ══════════════════════════════════════════════════════════════════════════════
// stdio_server_transport
// ══════════════════════════════════════════════════════════════════════════════

stdio_server_transport::stdio_server_transport()
    : impl_(std::make_unique<impl>(std::cin, std::cout)) {}

stdio_server_transport::stdio_server_transport(
    std::istream& in,
    std::ostream& out)
    : impl_(std::make_unique<impl>(in, out)) {}

stdio_server_transport::~stdio_server_transport() {
  stop();
}

stdio_server_transport::stdio_server_transport(
    stdio_server_transport&&) noexcept = default;
stdio_server_transport& stdio_server_transport::operator=(
    stdio_server_transport&&) noexcept = default;

void stdio_server_transport::start(bison::dynamic params) {
  if (!impl_ || !impl_->state)
    throw std::runtime_error("stdio_server_transport::start invalid state");
  if (impl_->reader_detached)
    throw std::runtime_error(
        "stdio_server_transport::start cannot restart after detached stop");

  stop();

  impl_->state->stop_requested.store(false);
  impl_->state->closed.store(false);
  impl_->state->hello_seen.store(false);
  impl_->state->negotiated_mode.store(
      static_cast<int>(framing_mode::auto_detect));
  reset_receive_state(*impl_->state);
  apply_common_params(*impl_->state, params);

#if defined(__linux__)
  (void)enable_server_tty_raw_mode(
      *impl_->state, impl_->tty_mode_active, impl_->saved_tty_mode);
#endif

  impl_->reader = std::thread(reader_loop, impl_->state);
  impl_->running.store(true);
  impl_->accepted.store(false);

  // In auto_detect mode, emit HELLO in both carriers so the client sees
  // whichever survives the channel.
  const std::string hello_body =
      std::string{kProtocolVersionToken} + ";type=HELLO";

  if (impl_->state->send_mode == framing_mode::auto_detect) {
    emit_control(*impl_->state, framing_mode::dcs,  hello_body);
    emit_control(*impl_->state, framing_mode::line, hello_body);
  } else {
    emit_control(*impl_->state, impl_->state->send_mode, hello_body);
  }
}

std::unique_ptr<server_connection_iface> stdio_server_transport::accept(
    std::chrono::milliseconds timeout) {
  if (!impl_ || !impl_->state || !impl_->running.load())
    return nullptr;

  // exchange(true) atomically sets accepted and returns the prior value.
  // If the prior value was true, someone else already accepted the connection.
  if (impl_->accepted.exchange(true)) {
    // Only one connection is ever supported per transport instance.
    // Block rather than spin so the caller's accept loop does not busy-wait;
    // wake early when stop() closes the channel.
    std::unique_lock<std::mutex> lk(impl_->state->read_mtx);
    impl_->state->read_cv.wait_for(
        lk, timeout, [this] { return impl_->state->closed.load(); });
    return nullptr;
  }

  return std::make_unique<stdio_server_connection>(
      std::make_unique<stdio_server_connection::impl>(impl_->state));
}

void stdio_server_transport::stop() {
  if (!impl_ || !impl_->state)
    return;

  impl_->state->stop_requested.store(true);
  impl_->state->closed.store(true);
  impl_->state->read_cv.notify_all();

  if (impl_->reader.joinable()) {
    // The reader may be blocked in a syscall; detach to avoid stop deadlock.
    // The transport is considered single-use after this point.
    impl_->reader.detach();
    impl_->reader_detached = true;
  }

  impl_->running.store(false);
  impl_->accepted.store(false);

#if defined(__linux__)
  restore_server_tty_mode(impl_->tty_mode_active, impl_->saved_tty_mode);
#endif
}

bool stdio_server_transport::wait_until_closed(
    std::chrono::milliseconds timeout) const {
  if (!impl_ || !impl_->state)
    return true;

  std::unique_lock<std::mutex> lk(impl_->state->read_mtx);
  return impl_->state->read_cv.wait_for(
      lk, timeout, [this] { return impl_->state->closed.load(); });
}

} // namespace bdg::bison::rmi::transport

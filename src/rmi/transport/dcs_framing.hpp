// MIT License © 2025 Binary Dice Games
/**
 * @file dcs_framing.hpp
 * @brief Shared DCS-over-PTY framing helpers used by pty_server_transport and
 *        pty_client_transport.
 *
 * This header is an internal implementation detail — it is not part of the
 * public bison API.  All definitions are inline or template so that no
 * additional translation unit is required.
 *
 * Linux and Windows.
 */
#pragma once

#if defined(__linux__) || defined(_WIN32)

#include "src/bison/bison.hpp"

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace bdg::bison::rmi::transport::dcs {

// ── Protocol constants ────────────────────────────────────────────────────────

inline constexpr char kEsc = '\x1b';
inline constexpr const char* kDcsStart = "\x1bP";
inline constexpr const char* kDcsEnd = "\x1b\\";
inline constexpr const char* kProtoVersion = "BISON_RMI/1";
inline constexpr const char* kTypeData = "DATA";
inline constexpr const char* kTypeHello = "HELLO";
inline constexpr const char* kTypeEnd = "END";

// ── Reassembly record ─────────────────────────────────────────────────────────

/** @brief Accumulator for one in-flight multi-chunk DATA message. */
struct partial_message {
  uint32_t total = 0;
  std::vector<std::optional<bison::buffer>> parts;
  size_t collected = 0;
  std::chrono::steady_clock::time_point first_seen;
};

// ── Low-level I/O helpers ─────────────────────────────────────────────────────

#if !defined(_WIN32)
/** @brief Write all @p size bytes from @p data to @p fd, retrying on EINTR. */
inline bool write_all_fd(int fd, const void* data, size_t size) {
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
#endif // !defined(_WIN32)

#if defined(_WIN32)
/** @brief Write all @p size bytes from @p data to Windows HANDLE @p h. */
inline bool write_all_fd(HANDLE h, const void* data, size_t size) {
  const auto* p = static_cast<const CHAR*>(data);
  DWORD rem = static_cast<DWORD>(size), off = 0;
  while (rem > 0) {
    DWORD w = 0;
    if (!WriteFile(h, p + off, rem, &w, nullptr) || w == 0)
      return false;
    off += w;
    rem -= w;
  }
  return true;
}
#endif // defined(_WIN32)

// ── Base-64 codec ─────────────────────────────────────────────────────────────

/** @brief Decode one RFC 4648 base64 character; returns -1 for invalid input. */
inline int from_b64(char c) {
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
inline std::string b64_encode(const uint8_t* data, size_t size) {
  static constexpr char kAlpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((size + 2) / 3) * 4);
  for (size_t i = 0; i < size; i += 3) {
    const uint32_t b0 = data[i];
    const uint32_t b1 = (i + 1 < size) ? data[i + 1] : 0u;
    const uint32_t b2 = (i + 2 < size) ? data[i + 2] : 0u;
    const uint32_t n = (b0 << 16) | (b1 << 8) | b2;
    out.push_back(kAlpha[(n >> 18) & 0x3f]);
    out.push_back(kAlpha[(n >> 12) & 0x3f]);
    out.push_back((i + 1 < size) ? kAlpha[(n >> 6) & 0x3f] : '=');
    out.push_back((i + 2 < size) ? kAlpha[n & 0x3f] : '=');
  }
  return out;
}

/**
 * @brief Decode RFC 4648 base64 @p input into @p out.
 * @return `true` on success; `false` on malformed input.
 */
inline bool b64_decode(const std::string& input, bison::buffer& out) {
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
    const int v2 = p2 ? 0 : from_b64(input[i + 2]);
    const int v3 = p3 ? 0 : from_b64(input[i + 3]);
    if ((!p2 && v2 < 0) || (!p3 && v3 < 0))
      return false;
    const uint32_t n = (static_cast<uint32_t>(v0) << 18) | (static_cast<uint32_t>(v1) << 12) |
        (static_cast<uint32_t>(v2) << 6) | static_cast<uint32_t>(v3);
    out.push_back(static_cast<uint8_t>((n >> 16) & 0xff));
    if (!p2)
      out.push_back(static_cast<uint8_t>((n >> 8) & 0xff));
    if (!p3)
      out.push_back(static_cast<uint8_t>(n & 0xff));
  }
  return true;
}

// ── Text helpers ──────────────────────────────────────────────────────────────

inline std::optional<uint64_t> parse_u64(const std::string& v) {
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

inline std::optional<uint32_t> parse_u32(const std::string& v) {
  const auto n = parse_u64(v);
  if (!n || *n > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()))
    return std::nullopt;
  return static_cast<uint32_t>(*n);
}

/**
 * @brief Parse a semicolon-delimited DCS body into key=value pairs.
 *
 * The first token must be `BISON_RMI/1`.  Returns an empty map if the version
 * token is missing or wrong.
 */
inline std::unordered_map<std::string, std::string> parse_fields(const std::string& body) {
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

// ── State helpers (templated — work with both server and client state structs) ─

/**
 * @brief Push a fully reassembled frame into the state's inbox and notify.
 *
 * @tparam State Must have: `read_mtx` (mutex), `inbox` (queue<buffer>),
 *               `read_cv` (condition_variable).
 */
template <typename State>
void push_inbound(State& st, bison::buffer frame) {
  {
    std::lock_guard<std::mutex> lk(st.read_mtx);
    st.inbox.push(std::move(frame));
  }
  st.read_cv.notify_all();
}

/**
 * @brief Mark the channel closed and wake all blocked receivers.
 *
 * @tparam State Must have: `closed` (atomic<bool>), `read_cv`.
 */
template <typename State>
void close_and_notify(State& st) {
  st.closed.store(true);
  st.read_cv.notify_all();
}

/**
 * @brief Remove any partial messages whose reassembly timeout has elapsed.
 *
 * @tparam State Must have: `pending` (map), `reassembly_timeout`.
 */
template <typename State>
void maybe_cleanup_expired(State& st) {
  const auto now = std::chrono::steady_clock::now();
  for (auto it = st.pending.begin(); it != st.pending.end();) {
    if (now - it->second.first_seen > st.reassembly_timeout)
      it = st.pending.erase(it);
    else
      ++it;
  }
}

/**
 * @brief Reassemble one DATA chunk into the pending map; deliver when complete.
 *
 * @tparam State Must expose the same fields as `pty_shared_state` /
 *               `client_state`: `read_mtx`, `pending`, `max_frame_bytes`, inbox.
 */
template <typename State>
void handle_data_frame(State& st, const std::unordered_map<std::string, std::string>& fields) {
  const auto it_id = fields.find("id");
  const auto it_seq = fields.find("seq");
  const auto it_total = fields.find("total");
  const auto it_b64 = fields.find("b64");

  if (it_id == fields.end() || it_seq == fields.end() || it_total == fields.end() || it_b64 == fields.end())
    return;

  const auto id = parse_u64(it_id->second);
  const auto seq = parse_u32(it_seq->second);
  const auto total = parse_u32(it_total->second);

  if (!id || !seq || !total || *total == 0 || *seq >= *total)
    return;

  bison::buffer chunk;
  if (!b64_decode(it_b64->second, chunk))
    return;

  bison::buffer assembled;
  {
    std::unique_lock<std::mutex> lk(st.read_mtx);
    maybe_cleanup_expired(st);

    auto& pm = st.pending[*id];
    if (pm.total == 0) {
      pm.total = *total;
      pm.parts.resize(*total);
      pm.collected = 0;
      pm.first_seen = std::chrono::steady_clock::now();
    }
    if (pm.total != *total) {
      st.pending.erase(*id);
      return;
    }

    auto& slot = pm.parts[*seq];
    if (slot.has_value())
      return;

    pm.collected += chunk.size();
    if (pm.collected > st.max_frame_bytes) {
      st.pending.erase(*id);
      return;
    }

    slot = std::move(chunk);

    for (const auto& part : pm.parts)
      if (!part.has_value())
        return;

    assembled.reserve(pm.collected);
    for (auto& part : pm.parts)
      assembled.insert(assembled.end(), part->begin(), part->end());
    st.pending.erase(*id);
  }
  push_inbound(st, std::move(assembled));
}

/**
 * @brief Dispatch a decoded DCS body: HELLO sets hello_seen, END closes, DATA
 *        is reassembled.
 */
template <typename State>
void process_body(State& st, const std::string& body) {
  const auto fields = parse_fields(body);
  if (fields.empty())
    return;
  const auto it_type = fields.find("type");
  if (it_type == fields.end())
    return;

  if (it_type->second == kTypeHello) {
    st.hello_seen.store(true);
    st.read_cv.notify_all();
    return;
  }
  if (it_type->second == kTypeEnd) {
    close_and_notify(st);
    return;
  }
  if (it_type->second == kTypeData)
    handle_data_frame(st, fields);
}

// ── Frame emission ────────────────────────────────────────────────────────────

/**
 * @brief Write one DCS frame (`ESC P <body> ESC \`) to @p fd under
 *        @p st.write_mtx.
 *
 * @tparam Fd   File descriptor type: `int` on Linux, `HANDLE` on Windows.
 * @tparam State  Must have: `write_mtx` (mutex).
 */
template <typename Fd, typename State>
void emit_dcs(Fd fd, State& st, const std::string& body) {
  std::lock_guard<std::mutex> lk(st.write_mtx);
  const std::string frame = std::string{kDcsStart} + body + kDcsEnd;
  if (!write_all_fd(fd, frame.data(), frame.size()))
    throw std::runtime_error("dcs_framing: write failed");
}

/**
 * @brief Split @p frame into chunks, base64-encode, and emit DATA frames.
 *
 * Uses @p st.write_mtx, @p st.next_msg_id, @p st.max_chunk_bytes, and
 * @p st.max_frame_bytes.
 *
 * @tparam Fd   File descriptor type: `int` on Linux, `HANDLE` on Windows.
 * @tparam State  Must expose the same fields as `pty_shared_state` / `client_state`.
 */
template <typename Fd, typename State>
void emit_data(Fd fd, State& st, const bison::buffer& frame) {
  if (frame.size() > st.max_frame_bytes)
    throw std::runtime_error("dcs_framing: frame exceeds max_frame_bytes");

  const size_t chunk = std::max<size_t>(1U, st.max_chunk_bytes);
  const uint64_t id = st.next_msg_id.fetch_add(1);

  if (frame.empty()) {
    emit_dcs(fd, st, std::string{kProtoVersion} + ";type=DATA;id=" + std::to_string(id) + ";seq=0;total=1;b64=");
    return;
  }

  const uint32_t total = static_cast<uint32_t>((frame.size() + chunk - 1) / chunk);

  for (uint32_t seq = 0; seq < total; ++seq) {
    const size_t off = static_cast<size_t>(seq) * chunk;
    const size_t len = std::min(chunk, frame.size() - off);
    const auto b64 = b64_encode(frame.data() + off, len);
    emit_dcs(
        fd,
        st,
        std::string{kProtoVersion} + ";type=DATA;id=" + std::to_string(id) + ";seq=" + std::to_string(seq) +
            ";total=" + std::to_string(total) + ";b64=" + b64);
  }
}

// ── DCS byte-level parser ─────────────────────────────────────────────────────

/**
 * @brief Stateful DCS escape-sequence parser.
 *
 * Feed one byte at a time via `feed()`.  When a complete DCS body is
 * accumulated the `on_frame` callback fires with the raw body string (the
 * content between `ESC P` and `ESC \`).  Bytes outside DCS blocks are routed
 * to the optional `on_plain` callback; pass an empty callback to discard them
 * silently (client-side behaviour).
 */
class dcs_byte_parser {
 public:
  using frame_cb = std::function<void(const std::string& body)>;
  using plain_cb = std::function<void(uint8_t c)>;

  /**
   * @param on_frame  Called with the complete DCS body on each terminator.
   * @param on_plain  Called for each non-DCS byte (leave empty to discard).
   */
  explicit dcs_byte_parser(frame_cb on_frame, plain_cb on_plain = {})
      : on_frame_(std::move(on_frame)), on_plain_(std::move(on_plain)) {}

  /** @brief Feed one byte into the parser. */
  void feed(uint8_t c) {
    const char ch = static_cast<char>(c);

    // ── Inside a DCS body ─────────────────────────────────────────────────
    if (in_dcs_) {
      if (dcs_saw_esc_) {
        if (ch == '\\') {
          if (on_frame_)
            on_frame_(dcs_buf_);
          dcs_buf_.clear();
          dcs_saw_esc_ = false;
          in_dcs_ = false;
          return;
        }
        // ESC not followed by \ — treat as literal ESC inside DCS body.
        dcs_buf_.push_back(kEsc);
        dcs_saw_esc_ = false;
      }
      if (ch == kEsc) {
        dcs_saw_esc_ = true;
        return;
      }
      dcs_buf_.push_back(ch);
      return;
    }

    // ── ESC lookahead (outside DCS) ───────────────────────────────────────
    if (saw_esc_) {
      saw_esc_ = false;
      if (ch == 'P') {
        in_dcs_ = true;
        dcs_buf_.clear();
        return;
      }
      // Unrecognised escape — emit the ESC as a plain byte then fall through.
      if (on_plain_)
        on_plain_(static_cast<uint8_t>(kEsc));
    }

    if (ch == kEsc) {
      saw_esc_ = true;
      return;
    }

    // ── Plaintext byte ────────────────────────────────────────────────────
    if (on_plain_)
      on_plain_(c);
  }

 private:
  bool in_dcs_ = false;
  bool saw_esc_ = false;
  bool dcs_saw_esc_ = false;
  std::string dcs_buf_;
  frame_cb on_frame_;
  plain_cb on_plain_;
};

} // namespace bdg::bison::rmi::transport::dcs

#endif // defined(__linux__) || defined(_WIN32)

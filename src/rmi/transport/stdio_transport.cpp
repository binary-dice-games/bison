// MIT License © 2025 Binary Dice Games
/**
 * @file stdio_transport.cpp
 * @brief Stdio stream transport implementation for RMI.
 */
#include "src/rmi/transport/stdio_transport.hpp"

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
#include <unistd.h>
#endif

namespace bdg::bison::rmi::transport {

namespace {

enum class framing_mode {
  auto_detect,
  dcs,
  line,
};

constexpr char kEsc = '\x1b';
constexpr const char* kDcsStart = "\x1bP";
constexpr const char* kDcsEnd = "\x1b\\";
constexpr const char* kLinePrefix = "@@BISON_RMI@@";
constexpr const char* kProtocolVersionToken = "BISON_RMI/1";
constexpr const char* kTypeData = "DATA";
constexpr const char* kTypeHello = "HELLO";
constexpr const char* kTypeEnd = "END";

struct partial_message {
  uint32_t total = 0;
  std::vector<std::optional<bison::buffer>> parts;
  size_t collected = 0;
  std::chrono::steady_clock::time_point first_seen;
};

struct shared_state {
  std::istream* in = nullptr;
  std::ostream* out = nullptr;

#if defined(__linux__)
  bool use_fds = false;
  int read_fd = -1;
  int write_fd = -1;
#endif

  std::mutex read_mtx;
  std::condition_variable read_cv;
  std::queue<bison::buffer> inbox;
  std::unordered_map<uint64_t, partial_message> pending;

  std::mutex write_mtx;

  std::atomic<bool> closed{false};
  std::atomic<bool> stop_requested{false};
  std::atomic<bool> hello_seen{false};

  framing_mode send_mode = framing_mode::dcs;
  framing_mode negotiated_mode = framing_mode::auto_detect;

  std::chrono::milliseconds handshake_timeout{5000};
  std::chrono::milliseconds reassembly_timeout{5000};
  size_t max_frame_bytes = 8U * 1024U * 1024U;
  size_t max_chunk_bytes = 1536U;
  bool mirror_plaintext_to_stderr = false;

  std::atomic<uint64_t> next_message_id{1};
};

int read_next_byte(shared_state& state) {
#if defined(__linux__)
  if (state.use_fds) {
    while (true) {
      uint8_t c = 0;
      const auto n = ::read(state.read_fd, &c, 1);
      if (n == 1) {
        return static_cast<int>(c);
      }
      if (n == 0) {
        return EOF;
      }
      if (errno == EINTR) {
        continue;
      }
      return EOF;
    }
  }
#endif

  if (!state.in) {
    return EOF;
  }
  return state.in->get();
}

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
      if (n < 0 && errno == EINTR) {
        continue;
      }
      return false;
    }
    return true;
  }
#endif

  if (!state.out) {
    return false;
  }
  (*state.out) << text;
  state.out->flush();
  return static_cast<bool>(*state.out);
}

bool starts_with(const std::string& value, const std::string& prefix) {
  return value.size() >= prefix.size() &&
      value.compare(0, prefix.size(), prefix) == 0;
}

std::string trim_copy(const std::string& input) {
  size_t begin = 0;
  while (begin < input.size() &&
         std::isspace(static_cast<unsigned char>(input[begin]))) {
    ++begin;
  }

  size_t end = input.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(input[end - 1]))) {
    --end;
  }
  return input.substr(begin, end - begin);
}

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

std::string base64_encode(const uint8_t* data, size_t size) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  std::string out;
  out.reserve(((size + 2) / 3) * 4);

  size_t i = 0;
  while (i < size) {
    const uint32_t b0 = data[i++];
    const uint32_t b1 = i < size ? data[i++] : 0;
    const uint32_t b2 = i < size ? data[i++] : 0;

    const uint32_t n = (b0 << 16) | (b1 << 8) | b2;
    out.push_back(kAlphabet[(n >> 18) & 0x3f]);
    out.push_back(kAlphabet[(n >> 12) & 0x3f]);
    out.push_back(i - 1 < size ? kAlphabet[(n >> 6) & 0x3f] : '=');
    out.push_back(i < size + 1 ? kAlphabet[n & 0x3f] : '=');
  }

  if (size % 3 == 1) {
    out[out.size() - 1] = '=';
    out[out.size() - 2] = '=';
  } else if (size % 3 == 2) {
    out[out.size() - 1] = '=';
  }

  return out;
}

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

    const int v2 = pad2 ? 0 : from_base64_char(c2);
    const int v3 = pad3 ? 0 : from_base64_char(c3);
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

std::optional<uint32_t> parse_u32(const std::string& value) {
  const auto n = parse_u64(value);
  if (!n)
    return std::nullopt;
  if (*n > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()))
    return std::nullopt;
  return static_cast<uint32_t>(*n);
}

std::optional<int32_t> parse_i32(const std::string& value) {
  try {
    size_t pos = 0;
    const auto parsed = std::stoll(value, &pos, 10);
    if (pos != value.size())
      return std::nullopt;
    if (parsed < std::numeric_limits<int32_t>::min() ||
        parsed > std::numeric_limits<int32_t>::max())
      return std::nullopt;
    return static_cast<int32_t>(parsed);
  } catch (...) {
    return std::nullopt;
  }
}

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

std::unordered_map<std::string, std::string> parse_fields(
    const std::string& body) {
  std::unordered_map<std::string, std::string> fields;

  std::stringstream ss(body);
  std::string token;
  bool first = true;
  while (std::getline(ss, token, ';')) {
    if (first) {
      first = false;
      if (token != kProtocolVersionToken) {
        return {};
      }
      continue;
    }

    const auto pos = token.find('=');
    if (pos == std::string::npos || pos == 0)
      continue;

    fields.emplace(token.substr(0, pos), token.substr(pos + 1));
  }

  return fields;
}

void maybe_cleanup_expired(shared_state& state) {
  const auto now = std::chrono::steady_clock::now();
  for (auto it = state.pending.begin(); it != state.pending.end();) {
    if (now - it->second.first_seen > state.reassembly_timeout) {
      it = state.pending.erase(it);
    } else {
      ++it;
    }
  }
}

void push_inbound(shared_state& state, bison::buffer frame) {
  {
    std::lock_guard<std::mutex> lk(state.read_mtx);
    state.inbox.push(std::move(frame));
  }
  state.read_cv.notify_all();
}

void close_and_notify(shared_state& state) {
  state.closed.store(true);
  state.read_cv.notify_all();
}

void handle_data_frame(
    shared_state& state,
    const std::unordered_map<std::string, std::string>& fields) {
  const auto it_id = fields.find("id");
  const auto it_seq = fields.find("seq");
  const auto it_total = fields.find("total");
  const auto it_b64 = fields.find("b64");

  if (it_id == fields.end() || it_seq == fields.end() ||
      it_total == fields.end() || it_b64 == fields.end()) {
    return;
  }

  const auto id = parse_u64(it_id->second);
  const auto seq = parse_u32(it_seq->second);
  const auto total = parse_u32(it_total->second);
  if (!id || !seq || !total || *total == 0 || *seq >= *total)
    return;

  bison::buffer chunk;
  if (!base64_decode(it_b64->second, chunk))
    return;

  std::lock_guard<std::mutex> lk(state.read_mtx);
  maybe_cleanup_expired(state);

  auto& pm = state.pending[*id];
  if (pm.total == 0) {
    pm.total = *total;
    pm.parts.resize(*total);
    pm.collected = 0;
    pm.first_seen = std::chrono::steady_clock::now();
  }

  if (pm.total != *total)
    return;

  auto& slot = pm.parts[*seq];
  if (slot.has_value())
    return;

  pm.collected += chunk.size();
  if (pm.collected > state.max_frame_bytes) {
    state.pending.erase(*id);
    return;
  }

  slot = std::move(chunk);

  bool complete = true;
  for (const auto& part : pm.parts) {
    if (!part.has_value()) {
      complete = false;
      break;
    }
  }

  if (!complete)
    return;

  bison::buffer frame;
  frame.reserve(pm.collected);
  for (auto& part : pm.parts) {
    frame.insert(frame.end(), part->begin(), part->end());
  }

  state.pending.erase(*id);
  state.inbox.push(std::move(frame));
  state.read_cv.notify_all();
}

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
    if (state.negotiated_mode == framing_mode::auto_detect) {
      state.negotiated_mode = mode;
    }
    state.hello_seen.store(true);
    state.read_cv.notify_all();
    return;
  }

  if (it_type->second == kTypeEnd) {
    close_and_notify(state);
    return;
  }

  if (it_type->second == kTypeData) {
    handle_data_frame(state, fields);
  }
}

void process_line(shared_state& state, const std::string& line) {
  auto trimmed = trim_copy(line);
  if (trimmed.empty())
    return;

  if (starts_with(trimmed, kLinePrefix)) {
    process_body(
        state,
        trimmed.substr(std::string{kLinePrefix}.size()),
        framing_mode::line);
    return;
  }

  if (state.mirror_plaintext_to_stderr) {
    std::cerr << trimmed << '\n';
  }
}

void reader_loop(std::shared_ptr<shared_state> state) {
  if (!state) {
    return;
  }

  bool has_input_source = (state->in != nullptr);
#if defined(__linux__)
  has_input_source = has_input_source || state->use_fds;
#endif
  if (!has_input_source) {
    return;
  }

  bool in_dcs = false;
  bool saw_esc = false;
  bool dcs_saw_esc = false;
  std::string line_buf;
  std::string dcs_buf;

  while (!state->stop_requested.load()) {
    const int ci = read_next_byte(*state);
    if (ci == EOF) {
      close_and_notify(*state);
      return;
    }

    const char c = static_cast<char>(ci);

    if (in_dcs) {
      if (dcs_saw_esc) {
        if (c == '\\') {
          process_body(*state, dcs_buf, framing_mode::dcs);
          dcs_buf.clear();
          dcs_saw_esc = false;
          in_dcs = false;
          continue;
        }
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

    if (saw_esc) {
      saw_esc = false;
      if (c == 'P') {
        in_dcs = true;
        dcs_buf.clear();
        continue;
      }
      line_buf.push_back(kEsc);
    }

    if (c == kEsc) {
      saw_esc = true;
      continue;
    }

    line_buf.push_back(c);
    if (c == '\n') {
      process_line(*state, line_buf);
      line_buf.clear();
    }
  }

  close_and_notify(*state);
}

void emit_control(
    shared_state& state,
    framing_mode mode,
    const std::string& body) {
  std::lock_guard<std::mutex> lk(state.write_mtx);

  std::string frame;
  if (mode == framing_mode::line) {
    frame = std::string{kLinePrefix} + body + "\n";
  } else {
    frame = std::string{kDcsStart} + body + kDcsEnd;
  }

  if (!write_all(state, frame)) {
    throw std::runtime_error("stdio transport write failed");
  }
}

framing_mode effective_mode(const shared_state& state) {
  if (state.send_mode == framing_mode::auto_detect) {
    if (state.negotiated_mode != framing_mode::auto_detect) {
      return state.negotiated_mode;
    }
    return framing_mode::dcs;
  }
  return state.send_mode;
}

void emit_data(shared_state& state, const bison::buffer& frame) {
  if (frame.size() > state.max_frame_bytes) {
    throw std::runtime_error("stdio transport frame exceeds max_frame_bytes");
  }

  const size_t chunk_size = std::max<size_t>(1U, state.max_chunk_bytes);
  const uint32_t total =
      static_cast<uint32_t>((frame.size() + chunk_size - 1) / chunk_size);
  const uint64_t msg_id = state.next_message_id.fetch_add(1);

  if (total == 0) {
    const std::string body = std::string{kProtocolVersionToken} +
        ";type=DATA;id=" + std::to_string(msg_id) + ";seq=0;total=1;b64=";
    emit_control(state, effective_mode(state), body);
    return;
  }

  for (uint32_t seq = 0; seq < total; ++seq) {
    const size_t offset = static_cast<size_t>(seq) * chunk_size;
    const size_t len = std::min(chunk_size, frame.size() - offset);
    const auto b64 = base64_encode(frame.data() + offset, len);

    const std::string body = std::string{kProtocolVersionToken} +
        ";type=DATA;id=" + std::to_string(msg_id) +
        ";seq=" + std::to_string(seq) + ";total=" + std::to_string(total) +
        ";b64=" + b64;

    emit_control(state, effective_mode(state), body);
  }
}

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

void wait_for_hello(shared_state& state) {
  if (state.send_mode == framing_mode::line) {
    state.negotiated_mode = framing_mode::line;
  }

  std::unique_lock<std::mutex> lk(state.read_mtx);
  const bool ok = state.read_cv.wait_for(lk, state.handshake_timeout, [&state] {
    return state.hello_seen.load() || state.closed.load();
  });

  if (!ok || !state.hello_seen.load()) {
    throw std::runtime_error("stdio_client_transport::open handshake timeout");
  }
}

} // namespace

struct stdio_client_transport::impl {
  explicit impl(std::istream& in, std::ostream& out)
      : state(std::make_shared<shared_state>()) {
    state->in = &in;
    state->out = &out;
  }

#if defined(__linux__)
  impl(int read_fd, int write_fd) : state(std::make_shared<shared_state>()) {
    state->use_fds = true;
    state->read_fd = read_fd;
    state->write_fd = write_fd;
  }
#endif

  std::shared_ptr<shared_state> state;
  std::thread reader;
  bool opened = false;
};

struct stdio_server_connection::impl {
  explicit impl(std::shared_ptr<shared_state> s) : state(std::move(s)) {}

  std::shared_ptr<shared_state> state;
};

struct stdio_server_transport::impl {
  explicit impl(std::istream& in, std::ostream& out)
      : state(std::make_shared<shared_state>()) {
    state->in = &in;
    state->out = &out;
  }

  std::shared_ptr<shared_state> state;
  std::thread reader;
  std::atomic<bool> running{false};
  std::atomic<bool> accepted{false};
};

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

void stdio_client_transport::open(bison::dynamic&& params) {
  if (!impl_ || !impl_->state) {
    throw std::runtime_error("stdio_client_transport::open invalid state");
  }

  impl_->state->stop_requested.store(false);
  impl_->state->closed.store(false);
  impl_->state->hello_seen.store(false);
  impl_->state->negotiated_mode = framing_mode::auto_detect;
  apply_common_params(*impl_->state, params);

  if (!impl_->reader.joinable()) {
    impl_->reader = std::thread(reader_loop, impl_->state);
  }

  wait_for_hello(*impl_->state);
  impl_->opened = true;
}

void stdio_client_transport::send(bison::buffer frame) {
  if (!impl_ || !impl_->state || !impl_->opened) {
    throw std::runtime_error("stdio_client_transport::send not open");
  }
  if (impl_->state->closed.load()) {
    throw std::runtime_error("stdio_client_transport::send closed");
  }
  emit_data(*impl_->state, frame);
}

bool stdio_client_transport::receive(
    bison::buffer& frame,
    std::chrono::milliseconds timeout) {
  if (!impl_ || !impl_->state || !impl_->opened) {
    return false;
  }

  std::unique_lock<std::mutex> lk(impl_->state->read_mtx);
  if (!impl_->state->read_cv.wait_for(lk, timeout, [this] {
        return !impl_->state->inbox.empty() || impl_->state->closed.load();
      })) {
    return false;
  }

  if (impl_->state->inbox.empty()) {
    return false;
  }

  frame = std::move(impl_->state->inbox.front());
  impl_->state->inbox.pop();
  return true;
}

void stdio_client_transport::shutdown() {
  if (!impl_ || !impl_->state) {
    return;
  }

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
    impl_->reader.detach();
  }

  impl_->opened = false;
}

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
  if (!impl_ || !impl_->state || impl_->state->closed.load()) {
    throw std::runtime_error("stdio_server_connection::send closed");
  }
  emit_data(*impl_->state, frame);
}

bool stdio_server_connection::receive(
    bison::buffer& frame,
    std::chrono::milliseconds timeout) {
  if (!impl_ || !impl_->state || impl_->state->closed.load()) {
    return false;
  }

  std::unique_lock<std::mutex> lk(impl_->state->read_mtx);
  if (!impl_->state->read_cv.wait_for(lk, timeout, [this] {
        return !impl_->state->inbox.empty() || impl_->state->closed.load();
      })) {
    return false;
  }

  if (impl_->state->inbox.empty()) {
    return false;
  }

  frame = std::move(impl_->state->inbox.front());
  impl_->state->inbox.pop();
  return true;
}

void stdio_server_connection::close() {
  if (!impl_ || !impl_->state) {
    return;
  }

  impl_->state->stop_requested.store(true);
  impl_->state->closed.store(true);
  impl_->state->read_cv.notify_all();
}

bool stdio_server_connection::is_closed() const {
  return !impl_ || !impl_->state || impl_->state->closed.load();
}

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
  if (!impl_ || !impl_->state) {
    throw std::runtime_error("stdio_server_transport::start invalid state");
  }

  stop();

  impl_->state->stop_requested.store(false);
  impl_->state->closed.store(false);
  impl_->state->hello_seen.store(false);
  impl_->state->negotiated_mode = framing_mode::auto_detect;
  apply_common_params(*impl_->state, params);

  impl_->reader = std::thread(reader_loop, impl_->state);
  impl_->running.store(true);
  impl_->accepted.store(false);

  const std::string hello_body =
      std::string{kProtocolVersionToken} + ";type=HELLO";

  if (impl_->state->send_mode == framing_mode::auto_detect) {
    emit_control(*impl_->state, framing_mode::dcs, hello_body);
    emit_control(*impl_->state, framing_mode::line, hello_body);
  } else {
    emit_control(*impl_->state, impl_->state->send_mode, hello_body);
  }
}

std::optional<stdio_server_connection> stdio_server_transport::accept(
    std::chrono::milliseconds timeout) {
  if (!impl_ || !impl_->state || !impl_->running.load()) {
    return std::nullopt;
  }

  if (impl_->accepted.load()) {
    std::this_thread::sleep_for(timeout);
    return std::nullopt;
  }

  impl_->accepted.store(true);
  return stdio_server_connection{
      std::make_unique<stdio_server_connection::impl>(impl_->state)};
}

void stdio_server_transport::stop() {
  if (!impl_ || !impl_->state) {
    return;
  }

  impl_->state->stop_requested.store(true);
  impl_->state->closed.store(true);
  impl_->state->read_cv.notify_all();

  if (impl_->reader.joinable()) {
    impl_->reader.detach();
  }

  impl_->running.store(false);
}

bool stdio_server_transport::wait_until_closed(
    std::chrono::milliseconds timeout) const {
  if (!impl_ || !impl_->state) {
    return true;
  }

  std::unique_lock<std::mutex> lk(impl_->state->read_mtx);
  return impl_->state->read_cv.wait_for(
      lk, timeout, [this] { return impl_->state->closed.load(); });
}

} // namespace bdg::bison::rmi::transport

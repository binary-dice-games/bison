// MIT License © 2025 Binary Dice Games
/**
 * @file tls_stream_state.hpp
 * @brief Per-connection TLS session state for TLS-secured TCP transports.
 *
 * `tls_stream_state<Handle>` wraps a plain `uv_stream_state<Handle>` (see
 * uv_stream_state.hpp) by composition, inserting an mbedTLS encrypt/decrypt
 * step between the raw socket bytes and the shared `frame_parser`
 * (frame_parser.hpp). `io`'s loop, handle, `send_async`/`stop_async`,
 * `stop()` (including its join/detach self-stop logic), and
 * `recv_queue`/`dequeue_frame()` are all reused unmodified; only the async
 * callbacks and the meaning of `send_queue` (repurposed to hold ciphertext,
 * via mbedTLS's own send callback rather than `io`'s own) differ, which is
 * why this composes `uv_stream_state` instead of extending it.
 *
 * All `mbedtls_ssl_*` calls happen only on this connection's own I/O loop
 * thread -- mirroring `uv_stream_state`'s "frame parser is loop-thread-only,
 * no locking needed" rule verbatim. Callers on other threads only ever touch
 * `plaintext_send_queue` (a `bison::synchronized<...>`, safe for cross-thread
 * push) via `enqueue_frame()`.
 *
 * See `src/rmi/DESIGN.md` §3.4/§13 for why mbedTLS was chosen and how this
 * layers under the transport-agnostic `auth_module_iface`.
 */
#pragma once

#include "src/bison/bison.hpp"
#include "src/rmi/transport/frame_parser.hpp"
#include "src/rmi/transport/mbedtls_threading.hpp"
#include "src/rmi/transport/uv_stream_state.hpp"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <uv.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace bdg::bison::rmi::transport {

/** @brief Server-side TLS configuration, parsed from `start()`'s `dynamic params`. */
struct tls_server_options {
  std::string cert_file, cert_pem;
  std::string key_file, key_pem;
  std::string key_password;
  std::string ca_file, ca_pem;
  /**
   * @brief Client-certificate verification mode.
   *
   * `"none"` (default): plain server-authenticated TLS, no client cert
   * requested. `"optional"`/`"required"` map directly onto
   * `mbedtls_ssl_conf_authmode()`; `"required"` fails the handshake if the
   * client doesn't present a certificate that verifies against `ca_file`/
   * `ca_pem`. See `src/rmi/DESIGN.md` §13 for the server-only-vs-mTLS
   * tradeoff this defaults against.
   */
  std::string client_auth = "none";
};

/** @brief Client-side TLS configuration, parsed from `open()`'s `dynamic params`. */
struct tls_client_options {
  /** SNI + hostname-verification target. Callers default this to `host`. */
  std::string server_name;
  std::string ca_file, ca_pem;
  /**
   * @brief Skip server certificate verification entirely.
   *
   * Unsafe for production use -- a dev/test escape hatch for self-signed
   * certificates, equivalent to `curl -k`. Prefer supplying `ca_file`/
   * `ca_pem` instead.
   */
  bool insecure_skip_verify = false;
  /** Optional client certificate/key, used only when the server requests mTLS. */
  std::string cert_file, cert_pem;
  std::string key_file, key_pem;
  std::string key_password;
};

namespace detail {

[[noreturn]] inline void throw_mbedtls_error(const std::string& what, int rc) {
  char buf[128];
  mbedtls_strerror(rc, buf, sizeof(buf));
  throw std::runtime_error(what + ": " + buf + " (" + std::to_string(rc) + ")");
}

inline void load_cert_chain(mbedtls_x509_crt& crt, const std::string& file, const std::string& pem, const char* what) {
  int rc = 0;
  if (!pem.empty())
    rc = mbedtls_x509_crt_parse(&crt, reinterpret_cast<const unsigned char*>(pem.c_str()), pem.size() + 1);
  else if (!file.empty())
    rc = mbedtls_x509_crt_parse_file(&crt, file.c_str());
  else
    throw std::runtime_error(std::string{"tls: no "} + what + " provided");
  if (rc != 0)
    throw_mbedtls_error(std::string{"tls: failed to parse "} + what, rc);
}

inline void load_private_key(mbedtls_pk_context& pk, const std::string& file, const std::string& pem,
                              const std::string& password, mbedtls_ctr_drbg_context& drbg) {
  const auto* pw = password.empty() ? nullptr : reinterpret_cast<const unsigned char*>(password.c_str());
  int rc = 0;
  if (!pem.empty())
    rc = mbedtls_pk_parse_key(&pk, reinterpret_cast<const unsigned char*>(pem.c_str()), pem.size() + 1, pw,
                               password.size(), mbedtls_ctr_drbg_random, &drbg);
  else if (!file.empty())
    rc = mbedtls_pk_parse_keyfile(&pk, file.c_str(), password.empty() ? nullptr : password.c_str(),
                                   mbedtls_ctr_drbg_random, &drbg);
  else
    throw std::runtime_error("tls: no private key provided");
  if (rc != 0)
    throw_mbedtls_error("tls: failed to parse private key", rc);
}

/**
 * @brief Force mbedTLS's lazily-computed ciphersuite table to be built
 *        exactly once, before any concurrent handshakes can race it.
 *
 * `mbedtls_ssl_list_ciphersuites()` (called internally by every
 * `mbedtls_ssl_config_defaults()` call) fills a file-static
 * `supported_ciphersuites` array on first use, guarded only by a plain
 * (non-atomic, unlocked) `static int supported_init` flag --
 * `extern/mbedtls/library/ssl_ciphersuites.c` has no `MBEDTLS_THREADING_C`
 * guard around it, unlike mbedTLS's PSA crypto key-slot state. Since bison
 * runs concurrent handshakes by design (a client's `open()` and a server's
 * per-connection accept thread both reach `mbedtls_ssl_config_defaults()`
 * around the same time -- see `src/rmi/DESIGN.md` §13), calling this once
 * up front lets C++11's thread-safe static-local initialization
 * ("magic statics") do the one-time build before any second caller can ever
 * observe a partially-filled table, without touching vendored mbedTLS code.
 */
inline void warm_ciphersuite_table() {
  static const int* const warmed = mbedtls_ssl_list_ciphersuites();
  (void)warmed;
}

inline int client_auth_to_mode(const std::string& mode) {
  if (mode == "none")
    return MBEDTLS_SSL_VERIFY_NONE;
  if (mode == "optional")
    return MBEDTLS_SSL_VERIFY_OPTIONAL;
  if (mode == "required")
    return MBEDTLS_SSL_VERIFY_REQUIRED;
  throw std::runtime_error("tls: invalid client_auth value '" + mode + "' (expected none, optional, or required)");
}

} // namespace detail

/**
 * @brief Validate that @p opts' certificate/key (and CA chain, if
 *        client-cert verification is enabled) parse successfully, without
 *        standing up a full TLS session or touching any libuv state.
 *
 * Used by `tls_socket_server_transport::start()` to fail fast on a bad
 * cert/key configuration instead of silently accepting connections whose
 * handshake can only ever fail.
 *
 * @throws std::runtime_error if any of the configured material fails to parse.
 */
inline void validate_server_options(const tls_server_options& opts) {
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_x509_crt cert, ca;
  mbedtls_pk_context key;
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&drbg);
  mbedtls_x509_crt_init(&cert);
  mbedtls_x509_crt_init(&ca);
  mbedtls_pk_init(&key);

  auto cleanup = [&] {
    mbedtls_pk_free(&key);
    mbedtls_x509_crt_free(&ca);
    mbedtls_x509_crt_free(&cert);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
  };

  try {
    const auto* pers = reinterpret_cast<const unsigned char*>("bison_tls_validate");
    const int rc = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, pers, 19);
    if (rc != 0)
      detail::throw_mbedtls_error("tls: failed to seed RNG", rc);
    detail::load_cert_chain(cert, opts.cert_file, opts.cert_pem, "server certificate");
    detail::load_private_key(key, opts.key_file, opts.key_pem, opts.key_password, drbg);
    const int authmode = detail::client_auth_to_mode(opts.client_auth);
    if (authmode != MBEDTLS_SSL_VERIFY_NONE)
      detail::load_cert_chain(ca, opts.ca_file, opts.ca_pem, "client CA chain");
  } catch (...) {
    cleanup();
    throw;
  }
  cleanup();
}

/**
 * @brief Per-connection TLS session state layered on top of a plain
 *        `uv_stream_state<Handle>` by composition.
 *
 * @tparam Handle  libuv stream handle type (`uv_tcp_t`).
 *
 * @par Usage pattern
 *  1. Default-construct, then `uv_loop_init(&st->io.loop)` + the
 *     handle-specific init call on `st->io.handle` (mirrors
 *     `uv_stream_state`'s own pattern).
 *  2. Call `configure_as_server()`/`configure_as_client()`.
 *  3. Call `init_asyncs()`.
 *  4. Client: `start_read()`, then `handshake_sync()` on the calling thread,
 *     then `start_loop()` once the handshake succeeds. Server: call
 *     `start_loop_with_handshake()` directly (runs handshake + steady state
 *     on one freshly spawned thread, off the shared accept thread -- see
 *     `src/rmi/DESIGN.md` §13).
 *  5. Use `enqueue_frame()`/`dequeue_frame()` from other threads.
 *  6. Call `stop()` (or let the destructor do it) to shut down.
 */
template <typename Handle>
struct tls_stream_state {
  uv_stream_state<Handle> io;
  frame_parser parser;

  mbedtls_entropy_context entropy{};
  mbedtls_ctr_drbg_context drbg{};
  mbedtls_x509_crt own_cert{};
  mbedtls_x509_crt ca_chain{};
  mbedtls_pk_context own_key{};
  mbedtls_ssl_config conf{};
  mbedtls_ssl_context ssl{};

  // Caller thread -> loop thread: plaintext, frame-prefixed bytes awaiting
  // encryption. `io.send_queue` is unused by this struct (mbedTLS's own send
  // callback, bio_send(), writes ciphertext straight out via uv_write()
  // instead of going through it).
  bison::synchronized<std::queue<std::vector<uint8_t>>> plaintext_send_queue;
  // Loop-thread-only staging buffer for mbedTLS's recv callback, bio_recv().
  std::vector<uint8_t> inbound_ciphertext;
  bool handshake_done{false};
  bool mbedtls_freed_{false};

  tls_stream_state() {
    ensure_mbedtls_threading_initialized();
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_x509_crt_init(&own_cert);
    mbedtls_x509_crt_init(&ca_chain);
    mbedtls_pk_init(&own_key);
    mbedtls_ssl_config_init(&conf);
    mbedtls_ssl_init(&ssl);
  }

  ~tls_stream_state() {
    stop();
  }

  tls_stream_state(const tls_stream_state&) = delete;
  tls_stream_state& operator=(const tls_stream_state&) = delete;
  tls_stream_state(tls_stream_state&&) = delete;
  tls_stream_state& operator=(tls_stream_state&&) = delete;

  // ── Configuration ──────────────────────────────────────────────────────────

  void seed_rng(const char* pers) {
    const int rc = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                          reinterpret_cast<const unsigned char*>(pers), std::strlen(pers));
    if (rc != 0)
      detail::throw_mbedtls_error("tls: failed to seed RNG", rc);
  }

  /** @brief Configure this connection as a TLS server endpoint. Call before the handshake. */
  void configure_as_server(const tls_server_options& opts) {
    detail::warm_ciphersuite_table();
    seed_rng("bison_tls_server");

    detail::load_cert_chain(own_cert, opts.cert_file, opts.cert_pem, "server certificate");
    detail::load_private_key(own_key, opts.key_file, opts.key_pem, opts.key_password, drbg);

    int rc = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0)
      detail::throw_mbedtls_error("tls: ssl_config_defaults failed", rc);

    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);

    const int authmode = detail::client_auth_to_mode(opts.client_auth);
    mbedtls_ssl_conf_authmode(&conf, authmode);
    if (authmode != MBEDTLS_SSL_VERIFY_NONE) {
      detail::load_cert_chain(ca_chain, opts.ca_file, opts.ca_pem, "client CA chain");
      mbedtls_ssl_conf_ca_chain(&conf, &ca_chain, nullptr);
    }

    rc = mbedtls_ssl_conf_own_cert(&conf, &own_cert, &own_key);
    if (rc != 0)
      detail::throw_mbedtls_error("tls: ssl_conf_own_cert failed", rc);

    rc = mbedtls_ssl_setup(&ssl, &conf);
    if (rc != 0)
      detail::throw_mbedtls_error("tls: ssl_setup failed", rc);

    wire_bio();
  }

  /** @brief Configure this connection as a TLS client endpoint. Call before the handshake. */
  void configure_as_client(const tls_client_options& opts) {
    detail::warm_ciphersuite_table();
    seed_rng("bison_tls_client");

    int rc = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0)
      detail::throw_mbedtls_error("tls: ssl_config_defaults failed", rc);

    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);

    if (opts.insecure_skip_verify) {
      mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
    } else {
      if (opts.ca_file.empty() && opts.ca_pem.empty())
        throw std::runtime_error(
            "tls: client requires ca_file or ca_pem to verify the server certificate "
            "(or insecure_skip_verify=true for testing only)");
      detail::load_cert_chain(ca_chain, opts.ca_file, opts.ca_pem, "server CA chain");
      mbedtls_ssl_conf_ca_chain(&conf, &ca_chain, nullptr);
      mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    }

    if (!opts.cert_file.empty() || !opts.cert_pem.empty()) {
      detail::load_cert_chain(own_cert, opts.cert_file, opts.cert_pem, "client certificate");
      detail::load_private_key(own_key, opts.key_file, opts.key_pem, opts.key_password, drbg);
      rc = mbedtls_ssl_conf_own_cert(&conf, &own_cert, &own_key);
      if (rc != 0)
        detail::throw_mbedtls_error("tls: ssl_conf_own_cert failed", rc);
    }

    rc = mbedtls_ssl_setup(&ssl, &conf);
    if (rc != 0)
      detail::throw_mbedtls_error("tls: ssl_setup failed", rc);

    if (!opts.server_name.empty()) {
      rc = mbedtls_ssl_set_hostname(&ssl, opts.server_name.c_str());
      if (rc != 0)
        detail::throw_mbedtls_error("tls: ssl_set_hostname failed", rc);
    }

    wire_bio();
  }

  // ── Lifecycle ──────────────────────────────────────────────────────────────

  /** @brief Wire up `send_async`/`stop_async` for the TLS read/write path. */
  void init_asyncs() {
    uv_async_init(&io.loop, &io.send_async, on_tls_send);
    io.send_async.data = this;
    // Reuses uv_stream_state::on_stop unchanged (it only closes io's own
    // handle/async handles, nothing TLS-specific), so stop_async.data must
    // stay a uv_stream_state<Handle>* -- point it at `io`, not `this`.
    uv_async_init(&io.loop, &io.stop_async, uv_stream_state<Handle>::on_stop);
    io.stop_async.data = &io;
    io.asyncs_ready = true;
  }

  /** @brief Start receiving raw bytes on `io.handle`. Call once, before `handshake_sync()`. */
  void start_read() {
    io.handle.data = this;
    uv_read_start(reinterpret_cast<uv_stream_t*>(&io.handle), alloc_cb, on_tls_read);
  }

  /**
   * @brief Run the blocking handshake on the calling thread, pumping
   *        `io.loop` with `UV_RUN_NOWAIT` between attempts -- mirrors the
   *        synchronous DNS/connect spin pattern in `socket_transport.cpp`.
   *
   * Must be called after `start_read()`, so incoming ciphertext bytes are
   * appended to `inbound_ciphertext` while the handshake is in progress.
   *
   * On the client path, call this *before* `init_asyncs()`: `io.stop()`
   * treats `asyncs_ready == true` as a promise that `start_loop()` will run
   * (so a stop signalled before the loop thread starts would otherwise be
   * silently dropped -- see `uv_stream_state::stop()`'s doc comment). Since
   * a handshake failure here throws, keeping `init_asyncs()` after this call
   * ensures a thrown exception leaves `asyncs_ready` false, so the
   * `tls_stream_state` destructor's `stop()` takes the synchronous
   * close-and-drain fallback instead of leaking the handle. The server path
   * (`start_loop_with_handshake()`) does not have this ordering constraint:
   * it never throws out to a caller, so it always leaves the handle in a
   * state a later `stop()` can close correctly (see that method's comment).
   *
   * @throws std::runtime_error on handshake failure, including certificate
   *         verification failure.
   */
  void handshake_sync() {
    for (;;) {
      const int rc = mbedtls_ssl_handshake(&ssl);
      if (rc == 0)
        break;
      if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
        uv_run(&io.loop, UV_RUN_NOWAIT);
        continue;
      }
      detail::throw_mbedtls_error("tls: handshake failed", rc);
    }
    handshake_done = true;
    pump_decrypt();
  }

  /** @brief Launch the background I/O thread for steady-state operation (post-handshake). */
  void start_loop() {
    io.loop_thread = std::thread([this] {
      uv_run(&io.loop, UV_RUN_DEFAULT);
      io.recv_closed.store(true);
      io.recv_queue.notify_all();
      uv_loop_close(&io.loop);
    });
  }

  /**
   * @brief Run the handshake and steady-state I/O loop entirely on a new
   *        dedicated thread, for the server accept path.
   *
   * Unlike the client (whose `open()` runs the handshake synchronously on
   * the caller's thread before spawning a background thread), a server
   * connection's handshake must not block the shared accept thread from
   * accepting the *next* connection -- see `src/rmi/DESIGN.md` §13.
   *
   * `init_asyncs()` must already have been called before this (unlike the
   * client path -- see `handshake_sync()`'s comment), since
   * `on_new_connection` pushes the connection into the accept queue right
   * after starting this thread, and a caller's `stop()`/`close()` racing the
   * handshake must be able to signal `stop_async` safely no matter how far
   * the handshake has gotten. A handshake failure here therefore does *not*
   * close the handle/loop directly (that would race a concurrent `stop()`
   * signalling the same, now-invalid `stop_async`); it only marks
   * `recv_closed` -- so callers see the connection as immediately closed --
   * and falls through to the same `uv_run(UV_RUN_DEFAULT)` tail as the
   * success path, which idles until an eventual `stop()` closes the handle
   * the normal way. There is no caller thread to throw an exception to
   * either way: `on_new_connection` has already returned by the time this
   * thread runs.
   */
  void start_loop_with_handshake() {
    io.loop_thread = std::thread([this] {
      try {
        start_read();
        handshake_sync();
      } catch (const std::exception&) {
        io.recv_closed.store(true);
        io.recv_queue.notify_all();
      }
      uv_run(&io.loop, UV_RUN_DEFAULT);
      io.recv_closed.store(true);
      io.recv_queue.notify_all();
      uv_loop_close(&io.loop);
    });
  }

  // ── Frame I/O ──────────────────────────────────────────────────────────────

  /** @brief Enqueue @p frame (frame-prefixed, plaintext) for encrypted send to the peer. */
  void enqueue_frame(const bison::buffer& frame) {
    std::vector<uint8_t> data(4 + frame.size());
    const uint32_t net_len = byte_swap(static_cast<uint32_t>(frame.size()));
    std::memcpy(data.data(), &net_len, 4);
    if (!frame.empty())
      std::memcpy(data.data() + 4, frame.data(), frame.size());
    plaintext_send_queue.withWLock([&](auto& q) { q.push(std::move(data)); });
    uv_async_send(&io.send_async);
  }

  /** @brief Block until a complete decrypted frame is available or the timeout elapses. */
  bool dequeue_frame(bison::buffer& frame, std::chrono::milliseconds timeout) {
    return io.dequeue_frame(frame, timeout);
  }

  /** @brief Signal the loop to stop, join/detach the background thread, and free TLS resources. */
  void stop() {
    io.stop();
    if (mbedtls_freed_)
      return;
    mbedtls_freed_ = true;
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_x509_crt_free(&own_cert);
    mbedtls_x509_crt_free(&ca_chain);
    mbedtls_pk_free(&own_key);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
  }

 private:
  void wire_bio() {
    mbedtls_ssl_set_bio(&ssl, this, bio_send, bio_recv, nullptr);
  }

  /** @brief Drain decrypted application data into `parser`, feeding complete frames to `io.recv_queue`. */
  void pump_decrypt() {
    uint8_t buf[16384];
    for (;;) {
      const int n = mbedtls_ssl_read(&ssl, buf, sizeof(buf));
      if (n > 0) {
        bison::buffer recycled;
        io.recycle_slot.withWLock([&](auto& spare) { recycled = std::move(spare); });
        if (recycled.capacity() > 0)
          parser.offer_reuse(std::move(recycled));

        const bool ok = parser.feed(buf, static_cast<size_t>(n), [this](bison::buffer&& frame) {
          io.recv_queue.withWLock([&](auto& q) { q.push(std::move(frame)); });
          io.recv_queue.notify_one();
        });
        if (!ok) {
          // Declared frame length exceeded frame_parser::kMaxFrameBytes --
          // fatal protocol error, same as a peer-initiated close.
          io.recv_closed.store(true);
          io.recv_queue.notify_all();
          return;
        }
        continue;
      }
      if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
        return;
      // 0, MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY, or any other error: treat as closed.
      io.recv_closed.store(true);
      io.recv_queue.notify_all();
      return;
    }
  }

  static void alloc_cb(uv_handle_t* h, size_t /*suggested*/, uv_buf_t* buf) {
    auto* st = static_cast<tls_stream_state*>(h->data);
    buf->base = reinterpret_cast<char*>(st->io.read_buf.data());
    buf->len = static_cast<decltype(buf->len)>(st->io.read_buf.size());
  }

  static void on_tls_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* /*buf*/) {
    auto* st = static_cast<tls_stream_state*>(stream->data);
    if (nread < 0) {
      st->io.recv_closed.store(true);
      st->io.recv_queue.notify_all();
      uv_read_stop(stream);
      return;
    }
    if (nread == 0)
      return;
    st->inbound_ciphertext.insert(st->inbound_ciphertext.end(), st->io.read_buf.data(),
                                   st->io.read_buf.data() + nread);
    if (st->handshake_done)
      st->pump_decrypt();
  }

  static void on_tls_send(uv_async_t* async) {
    auto* st = static_cast<tls_stream_state*>(async->data);
    std::queue<std::vector<uint8_t>> pending;
    st->plaintext_send_queue.withWLock([&](auto& q) { std::swap(pending, q); });
    while (!pending.empty()) {
      const auto& data = pending.front();
      size_t off = 0;
      while (off < data.size()) {
        const int n = mbedtls_ssl_write(&st->ssl, data.data() + off, data.size() - off);
        if (n > 0) {
          off += static_cast<size_t>(n);
          continue;
        }
        if (n == MBEDTLS_ERR_SSL_WANT_WRITE)
          continue; // bio_send is a non-blocking success against libuv's own write queue
        if (n == MBEDTLS_ERR_SSL_WANT_READ) {
          // Rare (e.g. a TLS 1.3 KeyUpdate mid-write): service any pending
          // read the write needs, then retry.
          uv_run(&st->io.loop, UV_RUN_NOWAIT);
          continue;
        }
        st->io.recv_closed.store(true);
        st->io.recv_queue.notify_all();
        return;
      }
      pending.pop();
    }
  }

  static int bio_send(void* ctx, const unsigned char* buf, size_t len) {
    auto* st = static_cast<tls_stream_state*>(ctx);
    auto* wr = new uv_write_req;
    wr->data.assign(buf, buf + len);
    uv_buf_t ub = uv_buf_init(reinterpret_cast<char*>(wr->data.data()), static_cast<unsigned>(wr->data.size()));
    const int rc = uv_write(&wr->req, reinterpret_cast<uv_stream_t*>(&st->io.handle), &ub, 1, on_uv_write_done);
    if (rc != 0) {
      delete wr;
      return -1;
    }
    return static_cast<int>(len);
  }

  static int bio_recv(void* ctx, unsigned char* buf, size_t len) {
    auto* st = static_cast<tls_stream_state*>(ctx);
    if (st->inbound_ciphertext.empty())
      return st->io.recv_closed.load() ? 0 : MBEDTLS_ERR_SSL_WANT_READ;
    const size_t n = std::min(len, st->inbound_ciphertext.size());
    std::memcpy(buf, st->inbound_ciphertext.data(), n);
    st->inbound_ciphertext.erase(st->inbound_ciphertext.begin(),
                                  st->inbound_ciphertext.begin() + static_cast<std::ptrdiff_t>(n));
    return static_cast<int>(n);
  }
};

} // namespace bdg::bison::rmi::transport

// MIT License © 2025 Binary Dice Games
/**
 * @file tls_socket_transport.hpp
 * @brief TLS-secured TCP socket transport implementation for RMI.
 *
 * A drop-in, TLS-encrypted sibling of `socket_transport` (see
 * socket_transport.hpp): same three transport interfaces
 * (`client_transport_iface`/`server_connection_iface`/
 * `server_transport_iface`), same 4-byte-BE-length-prefixed frame format on
 * the decrypted byte stream (FORMAT.md §5.1), same accept-time
 * socket-duplication dance -- the only difference is an mbedTLS handshake
 * and per-frame encrypt/decrypt step, implemented in tls_stream_state.hpp.
 *
 * By default the server authenticates itself to the client via a
 * certificate (like HTTPS); the app-level `auth_module_iface` hook
 * (src/rmi/server/auth.hpp) then authenticates the client over the now
 * confidential, server-authenticated channel. Mutual TLS (client
 * certificates) is available as an opt-in via `client_auth` for deployments
 * that want it -- see `src/rmi/DESIGN.md` §13 for the tradeoff this defaults
 * against.
 */
#pragma once

#include "src/bison/bison.hpp"
#include "src/rmi/transport/transport_iface.hpp"

#include <chrono>
#include <memory>
#include <string>

namespace bdg::bison::rmi::transport {

/**
 * @brief Client-side TLS-secured TCP transport endpoint.
 */
class tls_socket_client_transport : public client_transport_iface {
 public:
  /** @brief Construct a client endpoint with default host/port values. */
  tls_socket_client_transport(std::string host = "127.0.0.1", uint16_t port = 8443);
  /** @brief Close the endpoint if still open. */
  ~tls_socket_client_transport();

  tls_socket_client_transport(tls_socket_client_transport&&) noexcept;
  tls_socket_client_transport& operator=(tls_socket_client_transport&&) noexcept;

  tls_socket_client_transport(const tls_socket_client_transport&) = delete;
  tls_socket_client_transport& operator=(const tls_socket_client_transport&) = delete;

  /**
   * @brief Connect to the configured TCP endpoint and perform the TLS
   *        handshake. Does not return until the connection is fully usable.
   *
   * In addition to `host`/`port` overrides (see `socket_client_transport`),
   * accepts:
   *  - `server_name` (string, optional): SNI / hostname-verification target;
   *    defaults to `host`.
   *  - `ca_file` / `ca_pem` (string): trust anchor for verifying the
   *    server's certificate. Required unless `insecure_skip_verify` is set.
   *  - `insecure_skip_verify` (bool, default `false`): skip server
   *    certificate verification entirely. Unsafe for production.
   *  - `cert_file` / `cert_pem`, `key_file` / `key_pem`, `key_password`
   *    (string, optional): client certificate/key, used only when the
   *    server requests/accepts mutual TLS.
   *
   * @throws std::runtime_error on connection or TLS handshake failure
   *         (including certificate verification failure).
   */
  void open(bison::dynamic params) override;

  /** @brief Send one framed message to the server (encrypted). */
  void send(bison::buffer frame) override;

  /**
   * @brief Receive one framed message from the server (decrypted).
   * @param frame Output frame buffer.
   * @param timeout Maximum wait duration before returning `false`.
   * @return `true` when a frame was received, otherwise `false`.
   */
  bool receive(bison::buffer& frame, std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) override;

  /** @brief Shutdown and close the TLS/socket endpoint. */
  void shutdown() override;

  /** @brief Return false once the peer has closed the connection. */
  bool is_connected() const override;

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

/**
 * @brief Server-side TLS-secured TCP connection accepted from a listener.
 */
class tls_socket_server_connection : public server_connection_iface {
 public:
  /** @brief Construct an empty/closed connection state. */
  tls_socket_server_connection();
  /** @brief Close the connection if still active. */
  ~tls_socket_server_connection();

  tls_socket_server_connection(tls_socket_server_connection&&) noexcept;
  tls_socket_server_connection& operator=(tls_socket_server_connection&&) noexcept;

  tls_socket_server_connection(const tls_socket_server_connection&) = delete;
  tls_socket_server_connection& operator=(const tls_socket_server_connection&) = delete;

  /** @brief Send one framed message to the connected client (encrypted). */
  void send(bison::buffer frame) override;

  /**
   * @brief Receive one framed message from the connected client (decrypted).
   * @param frame Output frame buffer.
   * @param timeout Maximum wait duration before returning `false`.
   * @return `true` when a frame was received, otherwise `false`.
   */
  bool receive(bison::buffer& frame, std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) override;

  /** @brief Close this accepted connection. */
  void close() override;

  /** @brief Return whether the connection is closed. */
  bool is_closed() const override;

  struct impl;
  /** @brief Construct from an owned impl (used by tls_socket_server_transport). */
  explicit tls_socket_server_connection(std::unique_ptr<impl> impl);

 private:
  std::unique_ptr<impl> impl_;
};

/**
 * @brief TLS-secured TCP listener transport for server-side RMI endpoints.
 */
class tls_socket_server_transport : public server_transport_iface {
 public:
  /** @brief Construct a listener using default bind host/port values. */
  tls_socket_server_transport(std::string bind_host = "127.0.0.1", uint16_t port = 8443);
  /** @brief Stop listening and release listener resources. */
  ~tls_socket_server_transport();

  tls_socket_server_transport(tls_socket_server_transport&&) noexcept;
  tls_socket_server_transport& operator=(tls_socket_server_transport&&) noexcept;

  tls_socket_server_transport(const tls_socket_server_transport&) = delete;
  tls_socket_server_transport& operator=(const tls_socket_server_transport&) = delete;

  /**
   * @brief Start listening for incoming client TCP connections.
   *
   * In addition to `host`/`port` overrides (see `socket_server_transport`),
   * requires:
   *  - `cert_file` / `cert_pem` (string): server certificate chain.
   *  - `key_file` / `key_pem` (string): server private key.
   *  - `key_password` (string, optional): passphrase for an encrypted key.
   *
   * And accepts, for optional mutual TLS:
   *  - `client_auth` (string, default `"none"`): `"none"` | `"optional"` |
   *    `"required"`.
   *  - `ca_file` / `ca_pem` (string, required iff `client_auth != "none"`):
   *    trust anchor for verifying client certificates.
   *
   * The TLS handshake for each accepted connection runs on that
   * connection's own dedicated thread, not on the shared accept thread --
   * see `handshake_sync()`'s doc comment in tls_stream_state.hpp for why.
   *
   * @throws std::runtime_error if the certificate/key can't be loaded, or
   *         on bind/listen failure.
   */
  void start(bison::dynamic params) override;

  /**
   * @brief Create a client transport configured for this listener's
   *        host/port. The caller must still supply TLS trust material
   *        (`ca_file`/`ca_pem`, etc.) to the returned client's `open()` --
   *        the server and client may reasonably use different trust
   *        material, so it is not pre-populated here.
   */
  tls_socket_client_transport connect() const;

  /**
   * @brief Accept an incoming client connection.
   * @param timeout Maximum wait duration for the next connection.
   * @return Accepted server connection on success, otherwise `nullptr`.
   */
  std::unique_ptr<server_connection_iface> accept(
      std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) override;

  /** @brief Stop listening for new connections. */
  void stop() override;

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

} // namespace bdg::bison::rmi::transport

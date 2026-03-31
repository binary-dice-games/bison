// MIT License © 2025 Binary Dice Games
/**
 * @file server.hpp
 * @brief RMI server runtime that accepts connections and dispatches requests.
 */
#pragma once

#include "src/core/bison.hpp"
#include "src/rmi/server/context.hpp"
#include "src/rmi/shared/envelope.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace bdg::bison::rmi {

/**
 * @brief Hosts RMI objects over a transport and serves protocol operations.
 *
 * The server owns an accept loop and spawns worker threads for active
 * connections. Each request envelope is decoded and routed to the matching
 * operation handler (`connect`, `instantiate`, `call`, etc.).
 */
class server {
 public:
  // ── Template constructors (must stay in header for instantiation) ─────────

  /**
   * @brief Construct a server that references an external transport.
   * @tparam TTransport Transport type with `start/accept/stop` API.
   * @param transport Existing transport instance owned by the caller.
   */
  template <typename TTransport>
  explicit server(TTransport& transport)
      : transport_(
            std::make_unique<transport_ref_wrapper<TTransport>>(transport)) {}

  /**
   * @brief Construct a server by taking ownership of a transport.
   * @tparam TTransport Transport type with `start/accept/stop` API.
   * @param transport Transport instance moved into the server.
   */
  template <typename TTransport>
  explicit server(
      TTransport&& transport,
      std::enable_if_t<!std::is_lvalue_reference_v<TTransport>>* = nullptr)
      : transport_(
            std::make_unique<transport_val_wrapper<std::decay_t<TTransport>>>(
                std::move(transport))) {}

  server(const server&) = delete;
  server& operator=(const server&) = delete;
  server(server&&) = delete;
  server& operator=(server&&) = delete;

  ~server();

  /**
   * @brief Start listening for client connections.
   * @param params Optional transport-specific listen parameters.
   */
  void listen(bison::dynamic params = bison::dynamic{});

  /** @brief Stop accept loop, close active workers, and release resources. */
  void stop();

 private:
  // ── Inner interfaces ──────────────────────────────────────────────────────

  struct connection_iface {
    virtual void send(bison::buffer frame) = 0;
    virtual bool receive(bison::buffer&, std::chrono::milliseconds) = 0;
    virtual void close() = 0;
    virtual bool is_closed() const = 0;
    virtual ~connection_iface() = default;
  };

  template <typename C>
  struct connection_wrapper final : connection_iface {
    explicit connection_wrapper(C&& c) : c_(std::move(c)) {}
    void send(bison::buffer f) override {
      c_.send(std::move(f));
    }
    bool receive(bison::buffer& f, std::chrono::milliseconds to) override {
      return c_.receive(f, to);
    }
    void close() override {
      c_.close();
    }
    bool is_closed() const override {
      return c_.is_closed();
    }
    C c_;
  };

  struct transport_iface {
    virtual void start(bison::dynamic params) = 0;
    virtual std::unique_ptr<connection_iface> accept(
        std::chrono::milliseconds timeout) = 0;
    virtual void stop() = 0;
    virtual ~transport_iface() = default;
  };

  // Holds transport by reference (lvalue case).
  template <typename T>
  struct transport_ref_wrapper final : transport_iface {
    explicit transport_ref_wrapper(T& t) : t_(t) {}
    void start(bison::dynamic p) override {
      t_.start(std::move(p));
    }
    std::unique_ptr<connection_iface> accept(
        std::chrono::milliseconds to) override {
      auto maybe = t_.accept(to);
      if (!maybe)
        return nullptr;
      using ConnType = typename decltype(maybe)::value_type;
      return std::make_unique<connection_wrapper<ConnType>>(std::move(*maybe));
    }
    void stop() override {
      t_.stop();
    }
    T& t_;
  };

  // Holds transport by value (rvalue/ownership case).
  template <typename T>
  struct transport_val_wrapper final : transport_iface {
    explicit transport_val_wrapper(T&& t) : t_(std::move(t)) {}
    void start(bison::dynamic p) override {
      t_.start(std::move(p));
    }
    std::unique_ptr<connection_iface> accept(
        std::chrono::milliseconds to) override {
      auto maybe = t_.accept(to);
      if (!maybe)
        return nullptr;
      using ConnType = typename decltype(maybe)::value_type;
      return std::make_unique<connection_wrapper<ConnType>>(std::move(*maybe));
    }
    void stop() override {
      t_.stop();
    }
    T t_;
  };

  // ── Private methods (defined in server.cpp) ───────────────────────────────

  void accept_loop();
  void client_worker(std::unique_ptr<connection_iface> conn);
  void join_workers();

  static void send_response(
      connection_iface& conn,
      const shared::envelope& env,
      bison::key_t op,
      bison::dynamic payload);

  static void send_error(
      connection_iface& conn,
      const shared::envelope& env,
      bison::key_t op,
      bison::key_t code,
      const std::string& message);

  void handle_request(
      context& ctx,
      const shared::envelope& env,
      connection_iface& conn);
  static void handle_connect(
      context& ctx,
      const shared::envelope& env,
      connection_iface& conn);
  static void handle_describe(
      context& ctx,
      const shared::envelope& env,
      connection_iface& conn);
  static void handle_instantiate(
      context& ctx,
      const shared::envelope& env,
      connection_iface& conn);
  static void handle_clear(
      context& ctx,
      const shared::envelope& env,
      connection_iface& conn);
  static void
  handle_set(context& ctx, const shared::envelope& env, connection_iface& conn);
  static void
  handle_get(context& ctx, const shared::envelope& env, connection_iface& conn);
  static void handle_call(
      context& ctx,
      const shared::envelope& env,
      connection_iface& conn);
  static void handle_destroy(
      context& ctx,
      const shared::envelope& env,
      connection_iface& conn);
  static void handle_disconnect(
      context& ctx,
      const shared::envelope& env,
      connection_iface& conn);
  static void cleanup_context(context& ctx);

  // ── Members ───────────────────────────────────────────────────────────────

  std::unique_ptr<transport_iface> transport_;
  std::thread accept_thread_;
  std::atomic<bool> running_{false};

  bison::synchronized<std::vector<std::thread>> workers_;
};

} // namespace bdg::bison::rmi

// MIT License © 2025 Binary Dice Games
#pragma once

#include "src/core/bison.hpp"
#include "src/rmi/client/remote_dynamic.hpp"
#include "src/rmi/shared/constants.hpp"
#include "src/rmi/shared/envelope.hpp"
#include "src/rmi/shared/ids.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bdg::bison::rmi {

class client {
 public:
  template <typename TTransport>
  explicit client(TTransport&& transport)
      : transport_(std::make_unique<transport_wrapper<std::decay_t<TTransport>>>(
            std::forward<TTransport>(transport))) {}

  client(const client&)            = delete;
  client& operator=(const client&) = delete;
  client(client&&)                 = delete;
  client& operator=(client&&)      = delete;

  ~client();

  void            connect(bison::dynamic params = bison::dynamic{});
  bison::dynamic  describe(bison::key_t klass = 0U);
  remote::dynamic instantiate(bison::key_t klass,
                              bison::dynamic params = bison::dynamic{});
  void            destroy(remote::dynamic&& proxy);
  void            disconnect();

  std::future<bison::dynamic> send_request(bison::key_t       op,
                                            const std::string& object_id,
                                            bison::dynamic     payload,
                                            bool               oneway);

  void register_event_handler(const std::string&                  object_id,
                               bison::key_t                        name,
                               std::function<void(bison::dynamic)> handler);
  void unregister_object_events(const std::string& object_id);

 private:
  // ── Type-erased transport ─────────────────────────────────────────────────

  struct transport_iface {
    virtual void open(bison::dynamic params)                               = 0;
    virtual void send(std::vector<char> frame)                             = 0;
    virtual bool receive(std::vector<char>&,
                         std::chrono::milliseconds timeout)                = 0;
    virtual void shutdown()                                                = 0;
    virtual ~transport_iface()                                             = default;
  };

  template <typename T>
  struct transport_wrapper final : transport_iface {
    explicit transport_wrapper(T&& t) : t_(std::move(t)) {}
    void open(bison::dynamic p) override     { t_.open(std::move(p)); }
    void send(std::vector<char> f) override  { t_.send(std::move(f)); }
    bool receive(std::vector<char>& f,
                 std::chrono::milliseconds to) override {
      return t_.receive(f, to);
    }
    void shutdown() override { t_.shutdown(); }
    T t_;
  };

  // ── Private methods (defined in client.cpp) ───────────────────────────────

  void worker_loop();
  void process_frame(const bison::dynamic& env);
  void fail_all_pending(bison::key_t code, const std::string& message);

  // ── Members ───────────────────────────────────────────────────────────────

  std::unique_ptr<transport_iface> transport_;
  std::thread                      worker_;
  std::atomic<bool>                running_{false};

  std::mutex pending_mutex_;
  std::unordered_map<std::string, std::promise<bison::dynamic>> pending_;

  std::mutex event_mutex_;
  std::unordered_map<std::string,
                     std::unordered_map<bison::hash_t,
                                        std::function<void(bison::dynamic)>>>
      event_handlers_;

  std::mutex send_mutex_;
};

} // namespace bdg::bison::rmi

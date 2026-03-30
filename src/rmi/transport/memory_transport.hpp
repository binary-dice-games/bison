// MIT License © 2025 Binary Dice Games
#pragma once

#include "src/core/bison.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <vector>

namespace bdg::bison::rmi::transport {

struct memory_channel {
  std::mutex              mtx;
  std::condition_variable cv_c2s;
  std::condition_variable cv_s2c;
  std::queue<std::vector<char>> c2s_queue;
  std::queue<std::vector<char>> s2c_queue;
  std::atomic<bool>       closed{false};
};

class memory_client_transport {
 public:
  explicit memory_client_transport(std::shared_ptr<memory_channel> ch);
  void open(bison::dynamic params);
  void send(std::vector<char> frame);
  bool receive(std::vector<char>& frame,
               std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});
  void shutdown();
 private:
  std::shared_ptr<memory_channel> ch_;
};

class memory_server_connection {
 public:
  explicit memory_server_connection(std::shared_ptr<memory_channel> ch);
  void send(std::vector<char> frame);
  bool receive(std::vector<char>& frame,
               std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});
  void close();
  bool is_closed() const;
 private:
  std::shared_ptr<memory_channel> ch_;
};

class memory_server_transport {
 public:
  memory_server_transport() = default;
  memory_server_transport(const memory_server_transport&) = delete;
  memory_server_transport& operator=(const memory_server_transport&) = delete;

  void start(bison::dynamic params);
  memory_client_transport connect();
  std::optional<memory_server_connection> accept(
      std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});
  void stop();

 private:
  std::mutex                              mtx_;
  std::condition_variable                 cv_;
  std::queue<std::shared_ptr<memory_channel>> pending_;
  std::atomic<bool>                       stopped_{false};
};

} // namespace bdg::bison::rmi::transport

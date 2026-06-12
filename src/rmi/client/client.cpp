// MIT License © 2025 Binary Dice Games
/**
 * @file client.cpp
 * @brief Implementation of the RMI client runtime and remote proxy calls.
 */
#include "src/rmi/client/client.hpp"
#include "src/rmi/shared/schemas.hpp"

#include <stdexcept>

namespace bdg::bison::rmi {

using namespace shared::constants;

/** @brief Ensures active transport resources are shut down before destruction.
 */
client::~client() {
  if (running_.load()) {
    try {
      disconnect();
    } catch (...) {
    }
  }
}

/** @copydoc bdg::bison::rmi::client::connect */
void client::connect(bison::dynamic params) {
  shared::register_all_schemas();
  transport_->open(std::move(params));
  running_.store(true);
  worker_ = std::thread(&client::worker_loop, this);

  bison::dynamic payload;
  payload[FIELD_VERSION] = int32_t{PROTOCOL_VERSION};
  auto f = send_request(OP_CONNECT, {}, std::move(payload), false);
  f.get();
}

/** @copydoc bdg::bison::rmi::client::describe */
std::future<bison::dynamic> client::describe(bison::key_t klass) {
  bison::dynamic payload;
  payload[FIELD_KLASS] = klass;
  return send_request(OP_DESCRIBE, {}, std::move(payload), false);
}

/** @copydoc bdg::bison::rmi::client::instantiate */
std::future<proxy::dynamic> client::instantiate(
    bison::key_t ns,
    bison::key_t klass,
    bison::dynamic params) {
  bison::dynamic payload;
  payload[FIELD_KLASS] = klass;
  payload[FIELD_NAMESPACE] = ns;
  payload[FIELD_PARAMS] = bison::dynamic_ptr{std::move(params)};

  auto f = send_request(OP_INSTANTIATE, {}, std::move(payload), false);
  return std::async(std::launch::async, [this, f = std::move(f)]() mutable {
    auto result = f.get();
    bison::key_t oid = result.as<bison::key_t>(FIELD_OBJECT_ID);
    return proxy::dynamic{this, std::move(oid)};
  });
}

/** @copydoc bdg::bison::rmi::client::destroy */
void client::destroy(proxy::dynamic&& proxy) {
  bison::key_t oid = proxy.object_id();
  proxy.valid_ = false;
  proxy.client_ = nullptr;

  bison::dynamic payload;
  send_request(OP_DESTROY, oid, std::move(payload), false).get();
}

/** @copydoc bdg::bison::rmi::client::disconnect */
void client::disconnect() {
  if (!running_.load())
    return;

  bison::dynamic payload;
  try {
    send_request(OP_DISCONNECT, {}, std::move(payload), true).get();
  } catch (...) {
  }

  running_.store(false);
  transport_->shutdown();
  if (worker_.joinable())
    worker_.join();

  fail_all_pending(ERR_TRANSPORT_ERROR, "Client disconnected");
}

/** @copydoc bdg::bison::rmi::client::send_request */
std::future<bison::dynamic> client::send_request(
    bison::key_t op,
    bison::key_t object_id,
    bison::dynamic payload,
    bool oneway) {
  const bison::key_t request_id = shared::generate_id();
  auto frame = [&]() {
    shared::envelope env;
    env.kind = KIND_REQUEST;
    env.op = op;
    env.request_id = request_id;
    env.object_id = object_id;
    env.oneway = oneway;
    env.payload = std::move(payload);
    return env.encode();
  }();

  if (oneway) {
    {
      std::lock_guard<std::mutex> lk(send_mutex_);
      transport_->send(std::move(frame));
    }
    std::promise<bison::dynamic> p;
    p.set_value(bison::dynamic{});
    return p.get_future();
  }

  std::promise<bison::dynamic> promise;
  auto future = promise.get_future();
  pending_.wlock()->operator[](request_id.id) = std::move(promise);
  try {
    std::lock_guard<std::mutex> lk(send_mutex_);
    transport_->send(std::move(frame));
  } catch (...) {
    auto lp = pending_.wlock();
    auto it = lp->find(request_id.id);
    if (it != lp->end()) {
      it->second.set_exception(std::current_exception());
      lp->erase(it);
    }
    throw;
  }
  return future;
}

/** @copydoc bdg::bison::rmi::client::register_event_handler */
void client::register_event_handler(
    bison::key_t object_id,
    bison::key_t name,
    std::function<void(bison::dynamic)> handler) {
  event_handlers_.wlock()->operator[](object_id.id)[name.id] =
      std::move(handler);
}

/** @copydoc bdg::bison::rmi::client::unregister_object_events */
void client::unregister_object_events(bison::key_t object_id) {
  event_handlers_.wlock()->erase(object_id.id);
}

/**
 * @brief Receive loop that decodes frames and dispatches responses/events.
 */
void client::worker_loop() {
  while (running_.load(std::memory_order_acquire)) {
    bison::buffer frame;
    if (!transport_->receive(frame, std::chrono::milliseconds{50}))
      continue;
    try {
      auto env = shared::envelope::decode(frame);
      process_frame(env);
    } catch (...) {
    }
  }
  fail_all_pending(ERR_TRANSPORT_ERROR, "Worker thread exiting");
}

/**
 * @brief Route one decoded envelope to response resolution or event delivery.
 * @param env Decoded envelope object.
 */
void client::process_frame(const shared::envelope& env) {
  bison::key_t kind = env.kind;

  if (kind == KIND_RESPONSE) {
    bison::key_t request_id = env.request_id;

    std::promise<bison::dynamic> promise;
    {
      auto lp = pending_.wlock();
      auto it = lp->find(request_id.id);
      if (it == lp->end())
        return;
      promise = std::move(it->second);
      lp->erase(it);
    }

    const auto code = env.error.as<bison::key_t>(FIELD_ERROR_CODE);
    if (static_cast<bison::hash_t>(code) != 0u) {
      std::string message = "RMI error";
      if (const auto* msg = env.error.findField(FIELD_ERROR_MESSAGE);
          msg != nullptr) {
        message = msg->as<std::string>();
      }
      promise.set_exception(
          std::make_exception_ptr(
              std::runtime_error(
                  message + " (code=" + std::to_string(code.id) + ")")));
    } else {
      promise.set_value(env.payload.clone());
    }

  } else if (kind == KIND_EVENT) {
    bison::key_t object_id = env.object_id;
    auto& pval = env.payload;

    bison::key_t event_name = pval.as<bison::key_t>(FIELD_NAME);

    bison::dynamic params;
    auto& params_field = pval[FIELD_PARAMS];
    if (params_field.is<bison::dynamic_ptr>()) {
      auto ptr = params_field.as<bison::dynamic_ptr>();
      if (ptr)
        params = std::move(*ptr);
    }

    std::function<void(bison::dynamic)> handler;
    {
      auto lp = event_handlers_.rlock();
      auto oit = lp->find(object_id.id);
      if (oit != lp->end()) {
        auto eit = oit->second.find(event_name.id);
        if (eit != oit->second.end())
          handler = eit->second;
      }
    }
    if (handler) {
      try {
        handler(std::move(params));
      } catch (...) {
      }
    }
  }
}

/**
 * @brief Fail all unresolved requests with a transport-level RMI error.
 * @param code Error code token.
 * @param message Human-readable error message.
 */
void client::fail_all_pending(bison::key_t code, const std::string& message) {
  std::unordered_map<bison::hash_t, std::promise<bison::dynamic>> local;
  {
    auto lp = pending_.wlock();
    local = std::move(*lp);
  }
  for (auto& [id, promise] : local) {
    promise.set_exception(
        std::make_exception_ptr(
            std::runtime_error(
                message + " (code=" + std::to_string(code.id) + ")")));
  }
}

} // namespace bdg::bison::rmi

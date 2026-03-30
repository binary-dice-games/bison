// MIT License © 2025 Binary Dice Games
/**
 * @file client.cpp
 * @brief Implementation of the RMI client runtime and remote proxy calls.
 */
#include "src/rmi/client/client.hpp"

#include <stdexcept>

namespace bdg::bison::rmi {

using namespace shared::constants;

// ── Destructor ────────────────────────────────────────────────────────────────

/** @brief Ensures active transport resources are shut down before destruction. */
client::~client() {
  if (running_.load()) {
    try { disconnect(); } catch (...) {}
  }
}

// ── Public API ────────────────────────────────────────────────────────────────

/** @copydoc bdg::bison::rmi::client::connect */
void client::connect(bison::dynamic params) {
  shared::register_envelope();
  transport_->open(std::move(params));
  running_.store(true);
  worker_ = std::thread(&client::worker_loop, this);

  bison::dynamic payload;
  payload[FIELD_VERSION] = int32_t{PROTOCOL_VERSION};
  auto f = send_request(OP_CONNECT, {}, std::move(payload), false);
  f.get();
}

/** @copydoc bdg::bison::rmi::client::describe */
bison::dynamic client::describe(bison::key_t klass) {
  bison::dynamic payload;
  payload[FIELD_KLASS] = klass;
  return send_request(OP_DESCRIBE, {}, std::move(payload), false).get();
}

/** @copydoc bdg::bison::rmi::client::instantiate */
remote::dynamic client::instantiate(bison::key_t klass, bison::dynamic params) {
  bison::dynamic payload;
  payload[FIELD_KLASS]  = klass;
  payload[FIELD_PARAMS] = std::make_shared<bison::dynamic>(std::move(params));

  auto result  = send_request(OP_INSTANTIATE, {}, std::move(payload), false).get();
  std::string oid = result.as<std::string>(FIELD_OBJECT_ID);
  return remote::dynamic{this, std::move(oid)};
}

/** @copydoc bdg::bison::rmi::client::destroy */
void client::destroy(remote::dynamic&& proxy) {
  std::string oid = proxy.object_id();
  proxy.valid_  = false;
  proxy.client_ = nullptr;

  bison::dynamic payload;
  send_request(OP_DESTROY, oid, std::move(payload), false).get();
}

/** @copydoc bdg::bison::rmi::client::disconnect */
void client::disconnect() {
  if (!running_.load()) return;

  bison::dynamic payload;
  try {
    send_request(OP_DISCONNECT, {}, std::move(payload), true).get();
  } catch (...) {}

  running_.store(false);
  transport_->shutdown();
  if (worker_.joinable()) worker_.join();

  fail_all_pending(ERR_TRANSPORT_ERROR, "Client disconnected");
}

// ── Request dispatch ──────────────────────────────────────────────────────────

/** @copydoc bdg::bison::rmi::client::send_request */
std::future<bison::dynamic> client::send_request(bison::key_t       op,
                                                   const std::string& object_id,
                                                   bison::dynamic     payload,
                                                   bool               oneway) {
  const std::string request_id    = shared::generate_id();
  const std::string payload_bytes = shared::encode_payload(payload);
  auto frame = shared::encode_envelope(KIND_REQUEST, op, request_id,
                                       object_id, oneway, payload_bytes);

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
  {
    std::lock_guard<std::mutex> lk(pending_mutex_);
    pending_[request_id] = std::move(promise);
  }
  try {
    std::lock_guard<std::mutex> lk(send_mutex_);
    transport_->send(std::move(frame));
  } catch (...) {
    std::lock_guard<std::mutex> lk(pending_mutex_);
    auto it = pending_.find(request_id);
    if (it != pending_.end()) {
      it->second.set_exception(std::current_exception());
      pending_.erase(it);
    }
    throw;
  }
  return future;
}

/** @copydoc bdg::bison::rmi::client::register_event_handler */
void client::register_event_handler(const std::string&                  object_id,
                                     bison::key_t                        name,
                                     std::function<void(bison::dynamic)> handler) {
  std::lock_guard<std::mutex> lk(event_mutex_);
  event_handlers_[object_id][name.id] = std::move(handler);
}

/** @copydoc bdg::bison::rmi::client::unregister_object_events */
void client::unregister_object_events(const std::string& object_id) {
  std::lock_guard<std::mutex> lk(event_mutex_);
  event_handlers_.erase(object_id);
}

// ── Worker thread ─────────────────────────────────────────────────────────────

/**
 * @brief Receive loop that decodes frames and dispatches responses/events.
 */
void client::worker_loop() {
  while (running_.load(std::memory_order_acquire)) {
    std::vector<char> frame;
    if (!transport_->receive(frame, std::chrono::milliseconds{50})) continue;
    try {
      auto env = shared::decode_envelope(frame);
      process_frame(*env);
    } catch (...) {}
  }
  fail_all_pending(ERR_TRANSPORT_ERROR, "Worker thread exiting");
}

/**
 * @brief Route one decoded envelope to response resolution or event delivery.
 * @param env Decoded envelope object.
 */
void client::process_frame(const bison::dynamic& env) {
  bison::key_t kind = env.as<bison::key_t>(FIELD_KIND);

  if (kind == KIND_RESPONSE) {
    std::string request_id = env.as<std::string>(FIELD_REQUEST_ID);

    std::promise<bison::dynamic> promise;
    {
      std::lock_guard<std::mutex> lk(pending_mutex_);
      auto it = pending_.find(request_id);
      if (it == pending_.end()) return;
      promise = std::move(it->second);
      pending_.erase(it);
    }

    const std::string error_bytes = env.as<std::string>(FIELD_ERROR);
    if (!error_bytes.empty()) {
      auto error_obj = shared::decode_payload(error_bytes);
      bison::key_t code    = error_obj->as<bison::key_t>(FIELD_ERROR_CODE);
      std::string  message = error_obj->as<std::string>(FIELD_ERROR_MESSAGE);
      promise.set_exception(std::make_exception_ptr(
          shared::rmi_error{code, std::move(message)}));
    } else {
      const std::string payload_bytes = env.as<std::string>(FIELD_PAYLOAD);
      auto result = shared::decode_payload(payload_bytes);
      promise.set_value(std::move(*result));
    }

  } else if (kind == KIND_EVENT) {
    std::string object_id     = env.as<std::string>(FIELD_OBJECT_ID);
    std::string payload_bytes = env.as<std::string>(FIELD_PAYLOAD);
    auto        payload       = shared::decode_payload(payload_bytes);

    bison::key_t event_name = payload->as<bison::key_t>(FIELD_NAME);

    bison::dynamic params;
    auto& params_field = (*payload)[FIELD_PARAMS];
    if (params_field.is<std::shared_ptr<bison::dynamic>>()) {
      auto ptr = params_field.as<std::shared_ptr<bison::dynamic>>();
      if (ptr) params = std::move(*ptr);
    }

    std::function<void(bison::dynamic)> handler;
    {
      std::lock_guard<std::mutex> lk(event_mutex_);
      auto oit = event_handlers_.find(object_id);
      if (oit != event_handlers_.end()) {
        auto eit = oit->second.find(event_name.id);
        if (eit != oit->second.end()) handler = eit->second;
      }
    }
    if (handler) {
      try { handler(std::move(params)); } catch (...) {}
    }
  }
}

/**
 * @brief Fail all unresolved requests with a transport-level RMI error.
 * @param code Error code token.
 * @param message Human-readable error message.
 */
void client::fail_all_pending(bison::key_t code, const std::string& message) {
  std::unordered_map<std::string, std::promise<bison::dynamic>> local;
  {
    std::lock_guard<std::mutex> lk(pending_mutex_);
    local = std::move(pending_);
  }
  for (auto& [id, promise] : local) {
    promise.set_exception(std::make_exception_ptr(
        shared::rmi_error{code, message}));
  }
}

// ── remote::dynamic method bodies ────────────────────────────────────────────

namespace remote {

/** @copydoc bdg::bison::rmi::remote::dynamic::clear */
void dynamic::clear() {
  auto f = client_->send_request(
      shared::constants::OP_CLEAR, object_id_, bison::dynamic{}, false);
  f.get();
}

/** @copydoc bdg::bison::rmi::remote::dynamic::set */
void dynamic::set(bison::dynamic fields) {
  auto f = client_->send_request(
      shared::constants::OP_SET, object_id_, std::move(fields), false);
  f.get();
}

/** @copydoc bdg::bison::rmi::remote::dynamic::get */
void dynamic::get(bison::dynamic& fields) {
  bison::dynamic projection = fields;
  auto f = client_->send_request(
      shared::constants::OP_GET, object_id_, std::move(projection), false);
  fields = f.get();
}

/** @copydoc bdg::bison::rmi::remote::dynamic::call */
std::future<bison::dynamic> dynamic::call(bison::dynamic params, bool oneway) {
  return client_->send_request(
      shared::constants::OP_CALL, object_id_, std::move(params), oneway);
}

/** @copydoc bdg::bison::rmi::remote::dynamic::onEvent */
void dynamic::onEvent(bison::key_t name,
                       std::function<void(bison::dynamic)> handler) {
  client_->register_event_handler(object_id_, name, std::move(handler));
}

} // namespace remote
} // namespace bdg::bison::rmi

// MIT License © 2025 Binary Dice Games
/**
 * @file proxy.cpp
 * @brief Implementation of the RMI remote object proxy operations.
 */
#include "src/rmi/client/proxy.hpp"
#include "src/rmi/client/client.hpp"
#include "src/rmi/shared/constants.hpp"

#include <future>

namespace bdg::bison::rmi::proxy {

/** @copydoc bdg::bison::rmi::proxy::dynamic::clear */
std::future<bool> dynamic::clear() {
  auto f = backend_->send_request(shared::constants::OP_CLEAR, object_id_, bison::dynamic{}, false);
  return std::async(std::launch::async, [f = std::move(f)]() mutable {
    f.get();
    return true;
  });
}

/** @copydoc bdg::bison::rmi::proxy::dynamic::set */
std::future<bool> dynamic::set(bison::dynamic fields) {
  auto f = backend_->send_request(shared::constants::OP_SET, object_id_, std::move(fields), false);
  return std::async(std::launch::async, [f = std::move(f)]() mutable {
    f.get();
    return true;
  });
}

/** @copydoc bdg::bison::rmi::proxy::dynamic::get() */
std::future<bison::dynamic> dynamic::get() {
  return backend_->send_request(shared::constants::OP_GET, object_id_, bison::dynamic{}, false);
}

/** @copydoc bdg::bison::rmi::proxy::dynamic::get(bison::dynamic&&) */
std::future<bison::dynamic> dynamic::get(bison::dynamic&& projection) {
  return backend_->send_request(shared::constants::OP_GET, object_id_, std::move(projection), false);
}

/** @copydoc bdg::bison::rmi::proxy::dynamic::call */
std::future<bison::dynamic> dynamic::call(bison::key_t name, bison::dynamic&& params, bool oneway) {
  bison::dynamic payload;
  payload[shared::constants::FIELD_NAME] = name;
  payload[shared::constants::FIELD_PARAMS] = bison::dynamic_ptr{std::move(params)};
  return backend_->send_request(shared::constants::OP_CALL, object_id_, std::move(payload), oneway);
}

/** @copydoc bdg::bison::rmi::proxy::dynamic::onEvent */
void dynamic::onEvent(bison::key_t name, std::function<void(bison::dynamic)> handler) {
  backend_->register_event_handler(object_id_, name, std::move(handler));
}

/** @copydoc bdg::bison::rmi::proxy::dynamic::destroy */
void dynamic::destroy() {
  if (!valid_)
    return;
  bison::key_t oid = object_id_;
  proxy_backend* backend = backend_;
  valid_ = false;
  backend_ = nullptr;
  backend->destroy_object(oid);
}

} // namespace bdg::bison::rmi::proxy

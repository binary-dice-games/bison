// MIT License © 2025 Binary Dice Games
/**
 * @file rmi_c.cpp
 * @brief C++ implementation of the pure-C RMI shared-library API.
 *
 * Each exported function wraps one or more calls into the C++ RMI library.
 * All C++ exceptions are caught at the boundary and converted to `rmi_error`
 * return codes so that no exception can propagate through the C ABI.
 */

#include "rmi_c.h"
#include "rmi.hpp"

#include <cstring>
#include <memory>
#include <stdexcept>

using namespace bdg::bison;
using namespace bdg::bison::rmi;
using namespace bdg::bison::rmi::transport;

// ─── Internal helpers ──────────────────────────────────────────────────────

// For client: wrapper around std::unique_ptr<client>
using client_ptr = std::unique_ptr<client>;

/** Cast to client_ptr* */
static inline client_ptr* as_client_ptr(rmi_client_handle h) {
  return reinterpret_cast<client_ptr*>(h);
}

/** Cast from client_ptr* */
static inline rmi_client_handle as_client_handle(client_ptr* p) {
  return reinterpret_cast<rmi_client_handle>(p);
}

/** Dereference safely */
static inline client* client_deref(rmi_client_handle h) {
  if (!h)
    return nullptr;
  return as_client_ptr(h)->get();
}

// For server: wrapper around std::unique_ptr<server>
using server_ptr = std::unique_ptr<server>;

/** Cast to server_ptr* */
static inline server_ptr* as_server_ptr(rmi_server_handle h) {
  return reinterpret_cast<server_ptr*>(h);
}

/** Cast from server_ptr* */
static inline rmi_server_handle as_server_handle(server_ptr* p) {
  return reinterpret_cast<rmi_server_handle>(p);
}

/** Dereference safely */
static inline server* server_deref(rmi_server_handle h) {
  if (!h)
    return nullptr;
  return as_server_ptr(h)->get();
}

// For proxy: wrapper around std::unique_ptr<proxy::dynamic>
using proxy_ptr = std::unique_ptr<proxy::dynamic>;

/** Cast to proxy_ptr* */
static inline proxy_ptr* as_proxy_ptr(rmi_proxy_handle h) {
  return reinterpret_cast<proxy_ptr*>(h);
}

/** Cast from proxy_ptr* */
static inline rmi_proxy_handle as_proxy_handle(proxy_ptr* p) {
  return reinterpret_cast<rmi_proxy_handle>(p);
}

/** Dereference safely */
static inline proxy::dynamic* proxy_deref(rmi_proxy_handle h) {
  if (!h)
    return nullptr;
  return as_proxy_ptr(h)->get();
}

// Convert C++ shared_ptr<dynamic> to bison_handle
static inline bison_handle dynamic_to_bison_handle(
    const std::shared_ptr<dynamic>& sp) {
  auto* handle_ptr = new std::shared_ptr<dynamic>(sp);
  return reinterpret_cast<bison_handle>(handle_ptr);
}

// ─── Client ────────────────────────────────────────────────────────────────

RMI_API rmi_client_handle
rmi_client_tcp_create(const char* host, uint16_t port) {
  if (!host)
    return nullptr;
  try {
    socket_client_transport transport{host, port};
    auto* cp = new client_ptr(std::make_unique<client>(std::move(transport)));
    return as_client_handle(cp);
  } catch (...) {
    return nullptr;
  }
}

RMI_API rmi_error
rmi_client_connect(rmi_client_handle client, bison_handle params) {
  client* c = client_deref(client);
  if (!c)
    return RMI_ERR_NULL;
  try {
    dynamic dyn_params;
    if (params) {
      // Copy the bison_handle's dynamic object
      auto* sp = reinterpret_cast<std::shared_ptr<dynamic>*>(params);
      if (*sp)
        dyn_params = **sp;
    }
    c->connect(std::move(dyn_params));
    return RMI_OK;
  } catch (const std::runtime_error&) {
    return RMI_ERR_TRANSPORT;
  } catch (...) {
    return RMI_ERR_EXCEPTION;
  }
}

RMI_API rmi_error rmi_client_describe(
    rmi_client_handle client,
    uint32_t klass,
    bison_handle* out_desc) {
  if (!out_desc)
    return RMI_ERR_NULL;
  client* c = client_deref(client);
  if (!c)
    return RMI_ERR_NULL;
  try {
    dynamic desc = c->describe(key_t{klass});
    *out_desc = dynamic_to_bison_handle(std::make_shared<dynamic>(desc));
    return RMI_OK;
  } catch (const std::runtime_error&) {
    return RMI_ERR_INVALID_STATE;
  } catch (...) {
    return RMI_ERR_EXCEPTION;
  }
}

RMI_API rmi_error rmi_client_instantiate(
    rmi_client_handle client,
    uint32_t klass,
    bison_handle params,
    rmi_proxy_handle* out_proxy) {
  if (!out_proxy)
    return RMI_ERR_NULL;
  client* c = client_deref(client);
  if (!c)
    return RMI_ERR_NULL;
  try {
    dynamic dyn_params;
    if (params) {
      auto* sp = reinterpret_cast<std::shared_ptr<dynamic>*>(params);
      if (*sp)
        dyn_params = **sp;
    }
    proxy::dynamic proxy = c->instantiate(key_t{klass}, std::move(dyn_params));
    auto* pp =
        new proxy_ptr(std::make_unique<proxy::dynamic>(std::move(proxy)));
    *out_proxy = as_proxy_handle(pp);
    return RMI_OK;
  } catch (const std::runtime_error&) {
    return RMI_ERR_INVALID_STATE;
  } catch (...) {
    return RMI_ERR_EXCEPTION;
  }
}

RMI_API rmi_error rmi_client_disconnect(rmi_client_handle client) {
  client* c = client_deref(client);
  if (!c)
    return RMI_ERR_NULL;
  try {
    c->disconnect();
    return RMI_OK;
  } catch (...) {
    return RMI_ERR_EXCEPTION;
  }
}

RMI_API void rmi_client_release(rmi_client_handle client) {
  if (!client)
    return;
  try {
    client* c = client_deref(client);
    if (c)
      c->disconnect();
  } catch (...) {
    // Suppress exceptions during cleanup
  }
  delete as_client_ptr(client);
}

// ─── Proxy ────────────────────────────────────────────────────────────────

RMI_API void rmi_proxy_release(rmi_proxy_handle proxy) {
  if (!proxy)
    return;
  delete as_proxy_ptr(proxy);
}

RMI_API rmi_error rmi_proxy_call(
    rmi_client_handle client,
    rmi_proxy_handle proxy,
    uint32_t method,
    bison_handle params,
    bison_handle* out_result,
    int64_t timeout_ms) {
  client* c = client_deref(client);
  if (!c)
    return RMI_ERR_NULL;
  proxy::dynamic* px = proxy_deref(proxy);
  if (!px)
    return RMI_ERR_NULL;

  try {
    dynamic dyn_params;
    if (params) {
      auto* sp = reinterpret_cast<std::shared_ptr<dynamic>*>(params);
      if (*sp)
        dyn_params = **sp;
    }

    // Perform the call
    std::future<dynamic> fut = px->call(key_t{method}, std::move(dyn_params));

    // Wait for result with timeout
    std::chrono::milliseconds wait_time{timeout_ms < 0 ? 5000 : timeout_ms};
    auto wait_status = fut.wait_for(wait_time);

    if (wait_status == std::future_status::timeout)
      return RMI_ERR_TIMEOUT;

    dynamic result = fut.get();

    if (out_result) {
      *out_result = dynamic_to_bison_handle(std::make_shared<dynamic>(result));
    }
    return RMI_OK;
  } catch (const std::runtime_error& e) {
    // Check if it's a remote exception (look for common exception message
    // patterns)
    std::string msg = e.what();
    if (msg.find("remote") != std::string::npos ||
        msg.find("server") != std::string::npos)
      return RMI_ERR_REMOTE_EXCEPTION;
    return RMI_ERR_INVALID_STATE;
  } catch (...) {
    return RMI_ERR_EXCEPTION;
  }
}

// ─── Server ───────────────────────────────────────────────────────────────

RMI_API rmi_server_handle
rmi_server_tcp_create(const char* host, uint16_t port) {
  if (!host)
    return nullptr;
  try {
    socket_server_transport transport{host, port};
    auto* sp = new server_ptr(std::make_unique<server>(std::move(transport)));
    return as_server_handle(sp);
  } catch (...) {
    return nullptr;
  }
}

RMI_API rmi_error
rmi_server_listen(rmi_server_handle server, bison_handle params) {
  server* s = server_deref(server);
  if (!s)
    return RMI_ERR_NULL;
  try {
    dynamic dyn_params;
    if (params) {
      auto* sp = reinterpret_cast<std::shared_ptr<dynamic>*>(params);
      if (*sp)
        dyn_params = **sp;
    }
    s->listen(std::move(dyn_params));
    return RMI_OK;
  } catch (const std::runtime_error&) {
    return RMI_ERR_TRANSPORT;
  } catch (...) {
    return RMI_ERR_EXCEPTION;
  }
}

RMI_API void rmi_server_stop(rmi_server_handle server) {
  server* s = server_deref(server);
  if (!s)
    return;
  try {
    s->stop();
  } catch (...) {
    // Suppress exceptions during cleanup
  }
}

RMI_API void rmi_server_release(rmi_server_handle server) {
  if (!server)
    return;
  try {
    server* s = server_deref(server);
    if (s)
      s->stop();
  } catch (...) {
    // Suppress exceptions during cleanup
  }
  delete as_server_ptr(server);
}

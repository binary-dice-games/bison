// MIT License © 2025 Binary Dice Games
/**
 * @file auth.hpp
 * @brief Policy-free connection authentication hook for the RMI server.
 */
#pragma once

#include "src/bison/bison.hpp"
#include "src/rmi/server/context.hpp"

#include <memory>
#include <string>

namespace bdg::bison::rmi {

/**
 * @brief Optional authentication hook evaluated once per connection.
 *
 * Attach an implementation via `server::listen()`'s `auth_module` parameter.
 * bison attaches no semantics to the identity string it produces -- it is an
 * opaque, policy-free extension point; callers (e.g. wish) decide what an
 * identity means and what to do with it (see `server::on_authenticated`).
 */
class auth_module_iface {
 public:
  virtual ~auth_module_iface() = default;

  /**
   * @brief Evaluate a connection's `OP_CONNECT` payload.
   *
   * Called from `server::handle_connect()` once the client's connect payload
   * has arrived.
   *
   * @param ctx           Session context for the connecting client.
   * @param payload       The `OP_CONNECT` request payload (the `dynamic`
   *                      forwarded end-to-end from `client::connect()`).
   * @param out_identity  Set to a stable identity string for the caller to
   *                      use as it sees fit; left untouched (default empty)
   *                      if this module has no notion of identity for this
   *                      connection.
   * @return `true` to accept the session; `false` to reject it -- the server
   *         responds with `ERR_ACCESS_DENIED` and the client's `connect()`
   *         future throws.
   */
  virtual bool authenticate(context& ctx, const bison::dynamic& payload, std::string& out_identity) = 0;
};

using auth_module_ptr = std::shared_ptr<auth_module_iface>;

} // namespace bdg::bison::rmi

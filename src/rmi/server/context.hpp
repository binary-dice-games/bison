// MIT License © 2025 Binary Dice Games
/**
 * @file context.hpp
 * @brief Per-connection runtime state for server-side RMI request handling.
 */
#pragma once

#include "src/bison/bison.hpp"
#include "src/rmi/shared/ids.hpp"

#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace bdg::bison::rmi {

/**
 * @brief Mutable context owned by a server worker for one client session.
 *
 * Polymorphic: subclasses may attach application-specific per-session state.
 * See `server::on_create_context` for the extension point.
 */
struct context {
  context() = default;

  /** @brief Construct with a known session id. */
  explicit context(bison::key_t session_id) : session_id(session_id) {}

  virtual ~context() = default;

  /** @brief Unique session identifier for the connected client. */
  bison::key_t session_id;

  /** @brief Live object table keyed by remote object identifier token. */
  std::unordered_map<bison::hash_t, bison::dynamic_ptr> objects;

  /**
   * @brief Callback used by server logic to emit asynchronous events.
   *
   * Parameters are `(object_id, event_name, payload)`.
   */
  std::function<void(bison::key_t, bison::key_t, bison::dynamic)> emit_event;

  /**
   * @brief Which group `put_object()` files newly-created objects under.
   *
   * Set by `server::handle_request()` for the duration of dispatching one
   * request, from that request's envelope `group` field (see
   * `shared::envelope::group`). `0` means no group -- an object created
   * while this is `0` is only ever tracked in `objects`, matching behavior
   * before groups existed.
   *
   * This is what lets a group capture objects created *indirectly* -- e.g.
   * wish's UI template system creates a whole subtree of objects as a side
   * effect of one method call, not through individual `instantiate` round
   * trips -- as long as the code creating them calls `put_object()` (rather
   * than writing to `objects` directly) at some point in the same call
   * stack as the request that's currently being dispatched.
   */
  bison::hash_t current_group{0u};

  /**
   * @brief Object IDs filed under each group, keyed by group hash.
   *
   * Populated by `put_object()`; consumed (and erased) by
   * `server::handle_destroy_group()`. Purely a lifecycle grouping -- it has
   * no bearing on dispatch, lookup, or access to the objects in it, which
   * remain reachable through `objects` exactly like any other object.
   */
  std::unordered_map<bison::hash_t, std::unordered_set<bison::hash_t>> groups;

  /**
   * @brief File a newly-created object under `current_group` and insert it
   *        into `objects`.
   *
   * The single, canonical way to add an object to this context -- prefer
   * this over writing to `objects` directly so group membership (and
   * therefore group-based cleanup) stays correct.
   *
   * @param id  Newly-generated object identifier.
   * @param obj Object to store.
   */
  void put_object(bison::key_t id, bison::dynamic_ptr obj) {
    objects[id.id] = std::move(obj);
    if (current_group != 0u)
      groups[current_group].insert(id.id);
  }
};

} // namespace bdg::bison::rmi

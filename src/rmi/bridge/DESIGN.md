# `rmi::bridge` Design

## Overview

`rmi::bridge` multiplexes multiple downstream RMI clients into a single upstream
`rmi::server` session. It inherits from `rmi::server` (handling all downstream accept
and dispatch logic) and owns an `rmi::client` for the one upstream connection.

**Primary use case — remote desktop relay:**

```
Local machine                          Remote machine (via SSH)
─────────────────────                  ──────────────────────────────────────
wish::server (PTY mode)  ◄──pty──  rmi::bridge (PTY client side)
                                       └── pipe/TCP server side ◄── process A
                                                                 ◄── process B
```

The local wish::server renders a single session. The bridge relays all downstream
clients' bison commands into that session, multiplexing their UI together. The bridge
may inject its own objects (e.g. a desktop compositor widget) via the
`on_client_connected` override.

**Transport independence:** The server-side and client-side transports are chosen
independently at construction time. Any combination that satisfies
`transport::server_transport_iface` (downstream) and `transport::client_transport_iface`
(upstream) is valid — for example `pty_client_transport` upstream with
`socket_server_transport` downstream.

---

## Key abstractions

### `bridge` class

```
rmi::server  (base — manages downstream accept loop, worker threads, object table)
     ▲
rmi::bridge  (adds upstream rmi::client + relay state per downstream session)
     |
     └── rmi::client upstream_client_   (one connection to the upstream server)
```

### `session_state`

One `session_state` is created per downstream client connection. It owns:

| Field | Purpose |
|-------|---------|
| `session_id` | Mirrors `context::session_id` for the downstream session |
| `ns_prefix` | Unique `key_t` identifying this client's logical namespace |
| `emit` | Copy of `ctx.emit_event` — safe to call from the upstream event thread |
| `local_to_upstream` | `synchronized<map<local_oid_hash → upstream_oid>>` |
| `upstream_to_local` | `synchronized<map<upstream_oid_hash → local_oid>>` |

`session_state` is reference-counted (`shared_ptr`). Upstream event lambdas capture a
`weak_ptr<session_state>` so they become no-ops once the session is torn down.

### Proxy objects

`on_create_object(ctx, ns, klass)` is the only server hook that intercepts object
creation. When called, the bridge:

1. Calls `upstream_client_.instantiate(ns, klass, {}).get()` — synchronous, but
   the upstream client's worker thread is separate and can respond while the server
   worker blocks.
2. Returns a `bison::dynamic` **proxy object** with forwarding hooks registered.

The proxy object has no real fields; all operations are forwarded upstream:

| Hook / method | Forwards as |
|---------------|-------------|
| `CALL_FALLBACK` | `OP_CALL` with original method name + params |
| `HOOK_SETTER` | `OP_SET` with original payload |
| `HOOK_GETTER` | `OP_GET`, returns upstream result |
| `HOOK_DESTRUCT` | Cleans up translation tables; upstream destroy is done by `handle_destroy` |

`OP_CLEAR` is special: `handle_clear` resets the local object to a prototype clone,
destroying the proxy's hooks. The bridge intercepts OP_CLEAR in `on_request_trace`
(forwarding it upstream before local dispatch), then re-installs the forwarding proxy
in `on_response_trace` after `handle_clear` has run.

### Object ID translation

Downstream clients see only their own local object IDs (random `key_t` values assigned
by the server). The bridge maps each local ID to the corresponding upstream object ID
and back. The translation table is the trust boundary:

- A downstream client cannot construct a valid ID for another client's objects.
- The upstream server never sees downstream IDs.

### Namespace isolation

Each downstream session is assigned a unique `ns_prefix` key stored in `session_state`.
This logically scopes all objects created by that session. The isolation is currently
enforced at the bridge layer (object ID translation tables). Full bison-level namespace
separation — registering class aliases under per-session namespaces on the upstream —
is a future enhancement; `ns_prefix` is already in place to support it without API changes.

### Event routing

When upstream emits an event for an upstream object:

1. Upstream client's event dispatch thread fires the registered handler for that `upstream_oid`.
2. The handler looks up `weak_ptr<session_state>`; if the session is still alive,
   maps `upstream_oid → local_oid` and calls `emit(local_oid, event_name, payload)`.
3. `emit` is a copy of `ctx.emit_event`, which calls `conn->send()` (thread-safe
   connection-level mutex). No session or bridge lock is held in this path.

---

## Bison changes required

### 1. `CALL_FALLBACK` (in `bison_object.hpp`)

The bridge proxy must forward **any** method call without knowing the method names
ahead of time. This requires a catch-all fallback in `bison::dynamic`:

```cpp
/// Reserved method key: invoked when a named method is not found.
/// Receives { "__name": requested_name, "__params": dynamic_ptr{args} }.
static inline constexpr hash_t CALL_FALLBACK = "__callFallback"_key;
```

`dynamic::call(key_t name, const dynamic& params)` is modified: after failing
to find `name`, check for `CALL_FALLBACK` and invoke it with a wrapper carrying
the original name and params.  If absent, throw as before.

Purely additive and backward-compatible — no existing class registers `CALL_FALLBACK`.

### 2. `on_check_class` hook (in `server.hpp/server.cpp`)

`handle_instantiate` verifies that the requested class is present in the local
bison registry before calling `on_create_object`.  The bridge cannot pre-register
all upstream classes locally, so a new virtual guard is added:

```cpp
virtual bool on_check_class(context& ctx, key_t ns, key_t klass);
```

Default: checks the registry (existing behavior, fully backward-compatible).
`bridge::on_check_class` returns `true` unconditionally, letting every
instantiation request pass through to `on_create_object` where the bridge
forwards it to the upstream server.

### 3. Event routing without `register_event_handler`

The bison `client` dispatches events by exact `(object_id, event_name)` key
lookup — `name=0` is not a catch-all.  The bridge routes all upstream events
by intercepting them before the `rmi::client` ever sees them:

`bridge_upstream_transport` wraps the real upstream transport.  Its `receive()`
loop decodes each frame; event frames (`KIND_EVENT`) are forwarded to
`bridge::route_event()` and filtered out; all other frames are returned to the
`rmi::client` normally.  `route_event` uses the `upstream_to_session_` global
reverse-lookup table to find the downstream session and calls its `emit` function.

This requires no changes to `rmi::client`.

---

## Files

| File | Role |
|------|------|
| `src/bison/bison_object.hpp` | Add `CALL_FALLBACK` + fallback dispatch in `dynamic::call()` |
| `src/rmi/bridge/bridge.hpp` | `rmi::bridge` class declaration |
| `src/rmi/bridge/bridge.cpp` | `rmi::bridge` implementation |
| `src/rmi/rmi.hpp` | `#include "src/rmi/bridge/bridge.hpp"` |
| `tests/bridge_tests.cpp` | GoogleTest suite |
| `tests/CMakeLists.txt` | `package_add_test(bridge_test bridge_tests.cpp)` |

---

## Public API

```cpp
namespace bdg::bison::rmi {

class bridge : public server {
 public:
  // Borrowing: downstream transport is externally owned.
  bridge(transport::server_transport_iface& downstream,
         std::unique_ptr<transport::client_transport_iface> upstream_transport,
         bison::dynamic upstream_params = {});

  // Owning: bridge takes sole ownership of downstream transport.
  bridge(std::unique_ptr<transport::server_transport_iface> downstream,
         std::unique_ptr<transport::client_transport_iface> upstream_transport,
         bison::dynamic upstream_params = {});

  ~bridge() override;

  /// Connect to upstream, then start accepting downstream clients.
  void start(bison::dynamic downstream_params = {});

  /// Stop accepting, disconnect upstream, and shut down.
  void stop();

 protected:
  /// Called after a downstream client session is established.
  /// Override to inject bridge-owned UI objects (e.g. a desktop compositor).
  virtual void on_client_connected(context& ctx) { (void)ctx; }

  /// Called just before a downstream client session is torn down.
  virtual void on_client_disconnected(context& ctx) { (void)ctx; }

  // server hook overrides (see bridge.cpp for details)
  void on_session_created(context& ctx) override;
  void on_session_destroyed(context& ctx) override;
  bison::dynamic_ptr on_create_object(context&, bison::key_t ns,
                                       bison::key_t klass) override;
  void on_request_trace(context&, const shared::envelope&) override;
  void on_response_trace(context&, const shared::envelope& request_env,
                         bison::key_t op, bool is_error,
                         bison::key_t error_code,
                         const bison::dynamic& response_payload) override;
};

} // namespace bdg::bison::rmi
```

---

## Threading model

```
Server worker (one per downstream client)
  on_before_dispatch  ← no envelope
  on_request_trace    ← sets current_request_id_ (thread_local)
  handle_instantiate
    on_create_object  ← reads thread_local; calls upstream_client_.instantiate().get()
    send_response
      on_response_trace ← finalizes relay entry; registers upstream event handler

Upstream client worker thread
  Receives frames from upstream; resolves pending futures.

Upstream event dispatch thread
  Calls event handler lambdas (weak_ptr<session_state> guarded).
  Calls emit(local_oid, ...) → conn->send() [conn-level mutex, no bridge lock].
```

**Deadlock analysis:**

| Scenario | Analysis |
|----------|----------|
| Server worker blocks on `upstream_client_.send_request().get()` | Upstream worker thread responds independently; no shared lock held during block |
| Upstream event fires during OP_CALL | Event thread calls `emit` → `conn->send()` only; no session or bridge lock taken |
| `teardown_session` + concurrent event | `session_state` removed from map first; `weak_ptr` in event lambda expires; event becomes no-op |
| `pending_relays_` in `on_create_object` and `on_response_trace` | Sequential on same server worker thread; `synchronized<>` wlock is not nested |

---

## Dispatch hook sequence (OP_INSTANTIATE example)

```
client_worker reads frame
  on_before_dispatch(ctx)
  handle_request(ctx, env, conn)
    on_request_trace(ctx, env)          ← saves env.request_id → current_request_id_
    handle_instantiate(ctx, env, conn)
      on_create_object(ctx, ns, klass)  ← creates upstream object; stores pending_relay
      send_response(...)
        on_response_trace(...)          ← reads response_payload[FIELD_OBJECT_ID];
                                           finalizes tables; registers event handler
  on_after_dispatch(ctx)
```

---

## Invariants

1. Every upstream object created by the bridge is owned by exactly one downstream
   session's translation table. `teardown_session` destroys all of them on disconnect.
2. `session_state` is only accessible through `sessions_` (protected by `synchronized`)
   or via `weak_ptr` (which returns null after removal). No raw pointers to session state
   are stored outside these two paths.
3. `on_response_trace` for OP_CLEAR always runs on the same server worker thread as
   `on_request_trace`, so `pending_clear_` (thread_local) is coherent between the two.
4. The upstream client is connected before `server::listen()` is called. If `connect()`
   fails, `start()` throws and no downstream connections are accepted.

---

## Test coverage

Tests use `memory_server_transport` / `memory_client_transport` (in-process, synchronous).

| Test | Verifies |
|------|---------|
| Connect/disconnect | No crash, upstream session created and cleaned up |
| Object proxied upstream | `upstream.session_contexts()` shows object after instantiate; zero after destroy |
| SET + GET forwarded | Value stored on upstream object, returned to downstream client |
| Method call forwarded | `CALL_FALLBACK` routes call; upstream method executes; result returned |
| Events routed correctly | Upstream event for client A's object reaches A's handler only |
| Cross-client isolation | Client B cannot call method on client A's object ID (ERR_OBJECT_NOT_FOUND) |
| CLEAR re-installs proxy | Proxy still functional after OP_CLEAR; subsequent SET/GET work |
| Session teardown | Upstream `__destruct` hooks called for all objects on disconnect |

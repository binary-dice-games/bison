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

`term`-transport upstream is the reason the bridge multiplexes *one* upstream
connection across *many* downstream sessions rather than opening a dedicated
upstream connection per downstream client: a `term` connection wraps a single
fixed fd pair (a spawned pty or a process's own inherited stdio), so there is
no way to open a second, independent `term` connection to the same upstream
target. Any design has to work within that constraint.

---

## Key abstractions

### `bridge` class

```
rmi::server  (base — manages downstream accept loop, worker threads, dispatch)
     ▲
rmi::bridge  (adds upstream rmi::client + a pure-relay try_handle_request() override)
     |
     └── rmi::client upstream_client_   (one connection to the upstream server)
```

### Pure relay: no local object registry

Every object-touching request a downstream client sends — `instantiate`, `set`,
`get`, `call`, `clear`, `destroy` — is forwarded to the upstream server
**verbatim**: same object ID, same payload, uninspected. The upstream response is
relayed straight back. Object IDs are identical on both sides of the bridge; the
bridge never renumbers them and never stores a local mirror object for anything a
downstream client instantiates.

This is implemented via `server::try_handle_request()`, a hook that fires at the
very start of `handle_request()`, before the built-in `ctx.objects`-based
dispatch. `bridge::try_handle_request()` claims the six object-touching ops
above (forwarding them and calling `send_response()`/`send_error()` itself) and
returns `false` for everything else (`connect`, `describe`, `disconnect`,
`dictionary`, `help`), letting the base `server` handle those locally as usual.

**Why pure relay instead of mirroring proxy objects per instantiated object
(the previous design):** not every upstream object is created via a single
`OP_INSTANTIATE` round trip the bridge could intercept — e.g. wish's UI
template system creates a whole subtree of objects as a side effect of one
`OP_CALL` (`ui_template::do_instantiate`), inserting each one directly into the
*upstream* session's own object table. A scheme built around recognizing
`OP_INSTANTIATE` specifically (the earlier design: intercept instantiate,
mirror a local proxy object with `HOOK_SETTER`/`HOOK_GETTER`/`CALL_FALLBACK`/
`HOOK_DESTRUCT` forwarding hooks, track a local↔upstream ID translation table)
has no way to learn about objects created that way — any subsequent
`get`/`set`/`call` a downstream client issues against one fails with "Object
not found", because the bridge never mirrored it. Pure pass-through has no such
blind spot: it doesn't need to recognize object creation at all, so it doesn't
matter how many objects a single call produces upstream, or through what
mechanism.

### `session_state`

One `session_state` is created per downstream client connection. It owns:

| Field | Purpose |
|-------|---------|
| `session_id` | Mirrors `context::session_id` for the downstream session |
| `emit` | Copy of `ctx.emit_event` — safe to call from the upstream event thread |

`session_state` is reference-counted (`shared_ptr`), reachable only through the
`sessions_` map (protected by `synchronized`). Notably, it holds **no object
tracking at all** — see below.

### Disconnect cleanup: object groups, not bridge-side tracking

The bridge tracks nothing about individual objects, even for cleanup purposes.
Instead, `try_handle_request()` tags every relayed request with the downstream
session's own `ctx.session_id` as its **object group**
(`shared::envelope::group`, sent via `client::send_request_with_group()`).
On the upstream server, `context::current_group` is set from that field for
the duration of dispatching the request (`server::handle_request()`, generic —
not bridge-specific), and any object created while handling it — whether
directly via the built-in `OP_INSTANTIATE` handler, or indirectly as a side
effect of another op, e.g. wish's UI template system creating a whole subtree
of elements inside one `OP_CALL` — is filed under that group, *as long as the
creating code goes through `context::put_object()` instead of writing to
`context::objects` directly* (see `FORMAT.md` §4.1 and `context.hpp`'s doc
comments for the full mechanism; wish's own call sites — `ui_template.cpp`,
`forms/file_dialog.cpp`, and the calculator/notepad/process_explorer modules —
were all updated to use `put_object()` for exactly this reason).

`teardown_session()` then destroys everything in one request:
`send_request_with_group(OP_DESTROY_GROUP, {}, session_id, {}, oneway=true)`.
The upstream server's `handle_destroy_group()` looks up `context::groups[group]`,
runs `__destruct` on each member, erases them from `context::objects`, and
erases the group entry — a no-op if the group is empty or unknown, not an
error. This requires zero bridge-side bookkeeping and has no blind spot: it
doesn't matter how many objects a session's traffic produced, or through what
mechanism, because grouping happens at the point of *creation*, not by the
bridge guessing at ownership after the fact from request traffic it can see.

(An earlier iteration of this design had the bridge track a per-session
`touched_objects` set instead, populated from `try_handle_request()`'s own
view of `OP_INSTANTIATE` results and `env.object_id`. That missed objects
created indirectly — exactly wish's UI template case — since the bridge never
saw their IDs in anything it could recognize as "creation". Groups fix this at
the root: the *server* (which actually knows when an object comes into
existence, regardless of how) does the filing, not the bridge guessing from
outside.)

### Event routing: broadcast, not lookup

Server-initiated events are asynchronous — there's no in-flight request to
correlate an event against, so `try_handle_request()`'s per-request relay
doesn't help here. `route_event()` instead:

1. Delivers to the bridge's own upstream-client handler table
   (`upstream_client_.dispatch_local_event()`), for bridge-owned UI (e.g. a
   desktop compositor widget registered via `upstream()` in
   `on_client_connected()`).
2. Broadcasts to *every* connected downstream session's `emit`.

Broadcasting is safe and requires no per-object ownership tracking: each
session's own event-handler lookup (already unconditional on every ordinary
connection — see `client::process_frame`'s `KIND_EVENT` handling) silently
discards the frame if it has no handler registered for that object ID. An
object ID is a routing tag here, not a capability, so a session with no
interest in an object simply never reacts to its events, exactly as if the
bridge had never sent it at all.

`bridge_upstream_transport` (wraps the real upstream transport) is what makes
this possible: its `receive()` loop intercepts every `KIND_EVENT` frame before
handing frames to `rmi::client`, so `route_event()` sees every upstream event
regardless of whether the bridge's own `upstream_client_` has any handler
registered for it.

### Object IDs are not a security boundary

The earlier design's local↔upstream ID translation tables had an incidental
side effect: a downstream client referencing another client's object ID would
fail with "Object not found", since the bridge's own per-session object
registry had no entry for it. Pure relay removes that translation layer, so
that side effect is gone — a downstream client that knows (or guesses) another
session's object ID can now successfully operate on it, forwarded like any
other request.

This was never a deliberate access-control boundary: `on_check_class()`
already accepts any class from any downstream client with no validation at
all, so the bridge doesn't otherwise attempt to isolate downstream sessions
from each other. `tests/bridge_tests.cpp`'s
`ObjectIdsAreRoutingTagsNotCapabilities` test documents this explicitly.

---

## Bison changes required (historical — kept for context)

These were introduced for the earlier proxy-based design and are no longer
exercised by `bridge` itself, but remain in bison as general-purpose,
backward-compatible additions other code may still rely on:

### `CALL_FALLBACK` (in `bison_object.hpp`)

Reserved method key: invoked when a named method is not found. Receives
`{ "__name": requested_name, "__params": dynamic_ptr{args} }`. Purely additive;
no core dispatch path requires it.

### `on_check_class` hook (in `server.hpp`/`server.cpp`)

```cpp
virtual bool on_check_class(context& ctx, key_t ns, key_t klass);
```

Default: checks the registry (existing behavior). No longer overridden by
`bridge` (pure relay bypasses `handle_instantiate` entirely via
`try_handle_request()`), but still available to other `server` subclasses.

---

## Current bison changes required

### `server::try_handle_request` hook (in `server.hpp`/`server.cpp`)

```cpp
virtual bool try_handle_request(context& ctx, const shared::envelope& env,
                                 transport::server_connection_iface& conn) {
  return false; // default: fall through to the built-in ctx.objects dispatch
}
```

Called at the very start of `handle_request()`, before `on_request_trace()`
and before the `ctx.objects`-based `OP_*` dispatch table. If it returns `true`,
the override must itself have sent a response or error (via `send_response()`/
`send_error()`, both now `protected` rather than `private` so a subclass can
call them) and `handle_request()` returns immediately. This is what lets
`bridge` implement request handling that doesn't fit the per-connection
`ctx.objects` model at all.

### Object groups (in `context.hpp`, `server.hpp`/`server.cpp`, `envelope.hpp`/`.cpp`, `client.hpp`/`.cpp`, `constants.hpp`, `schemas.hpp`)

A general-purpose, protocol-level lifecycle-grouping mechanism, not specific
to `bridge`:

- **`shared::envelope::group`** (`bison::key_t`, new field, wire tag `__group`
  — see `FORMAT.md` §4.1): carried on every request; `0` means "no group".
- **`context::current_group`**: set from `env.group` by `server::handle_request()`
  for the duration of dispatching one request (generic, any `server`).
- **`context::groups`**: `unordered_map<group_hash, unordered_set<object_id_hash>>`,
  populated by...
- **`context::put_object(id, obj)`**: the canonical way to add an object to a
  context — inserts into `context::objects` *and* (if `current_group != 0`)
  `context::groups[current_group]`. Prefer this over writing to `objects`
  directly anywhere an object might be created under a group.
- **`client::send_request_with_group(op, object_id, group, payload, oneway)`**:
  identical to the existing `send_request()`, plus a `group` parameter.
  `send_request()` itself is now a thin wrapper calling this with `group = 0`
  — fully backward compatible, no existing call site changed behavior.
- **`OP_DESTROY_GROUP`** / **`server::handle_destroy_group()`**: destroys
  every object in `context::groups[env.group]` (running `__destruct` on each),
  then erases the group. No-op (not an error) if the group is empty/unknown.

None of this is bridge-specific — any `server` subclass, or even ordinary
application code creating objects in bulk, can use groups. `bridge` is simply
the first (and so far only) consumer, using each downstream session's own
`session_id` as its group key on the shared upstream connection.

**Wire compatibility:** this is a real (additive) wire-format change — see
`FORMAT.md` §4.1. Since bison is vendored/single-versioned in this monorepo
(not independently deployed against older peers), no backward-compatibility
shim was added; rebuild both sides together.

---

## Files

| File | Role |
|------|------|
| `src/rmi/server/context.hpp` | `current_group`, `groups`, `put_object()` |
| `src/rmi/server/server.hpp` / `.cpp` | `try_handle_request()` hook; `send_response()`/`send_error()` made `protected`; sets `ctx.current_group`; `handle_destroy_group()` |
| `src/rmi/shared/envelope.hpp` / `.cpp` | `group` field, encode/decode |
| `src/rmi/shared/constants.hpp` | `FIELD_GROUP`, `OP_DESTROY_GROUP` |
| `src/rmi/shared/schemas.hpp` | `__envelope` schema gains `__group` |
| `src/rmi/client/client.hpp` / `.cpp` | `send_request_with_group()` |
| `src/rmi/bridge/bridge.hpp` | `rmi::bridge` class declaration |
| `src/rmi/bridge/bridge.cpp` | `rmi::bridge` implementation |
| `src/rmi/rmi.hpp` | `#include "src/rmi/bridge/bridge.hpp"` |
| `tests/bridge_tests.cpp` | GoogleTest suite |
| `tests/CMakeLists.txt` | `package_add_test(bridge_test bridge_tests.cpp)` |

wish-side call sites updated to use `put_object()` instead of writing to
`ctx.objects` directly (so bulk-created elements are correctly grouped when
relayed through a bridge): `src/ui/ui_template.cpp`, `src/ui/forms/file_dialog.cpp`,
`modules/calculator/server/calculator.cpp`, `modules/notepad/server/notepad.cpp`,
`modules/process_explorer/server/process_explorer.cpp`.

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

  ~bridge();

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

  rmi::client& upstream(); // access the shared upstream connection

  // server hook overrides (see bridge.cpp for details)
  void on_session_created(context& ctx) override;
  void on_session_destroyed(context& ctx) override;
  bool try_handle_request(context& ctx, const shared::envelope& env,
                           transport::server_connection_iface& conn) override;
};

} // namespace bdg::bison::rmi
```

---

## Threading model

```
Server worker (one per downstream client)
  on_before_dispatch          ← no envelope
  handle_request
    try_handle_request(ctx, env, conn)
      upstream_client_.send_request_with_group(op, env.object_id, ctx.session_id, env.payload, oneway).get()
        ← blocks this worker; the upstream client's own worker thread
          responds independently, no shared lock held during the block
      send_response(...) / send_error(...)
  on_after_dispatch

Upstream server's own worker thread (a separate process/connection entirely)
  handle_request: ctx.current_group = env.group, then normal dispatch
    handle_instantiate / a method like ui_template's "instantiate"
      ctx.put_object(new_id, obj) ← files under ctx.current_group

Upstream client worker thread
  Receives frames from upstream; resolves pending futures.

Upstream event dispatch thread (bridge_upstream_transport::receive())
  Intercepts KIND_EVENT frames before rmi::client sees them.
  route_event(env):
    upstream_client_.dispatch_local_event(env)   ← bridge-owned UI
    for each session in sessions_: emit(...) under that session's emit_st lock
```

**Deadlock analysis:**

| Scenario | Analysis |
|----------|----------|
| Server worker blocks on `upstream_client_.send_request().get()` in `try_handle_request` | Upstream worker thread responds independently; no shared lock held during block |
| Upstream event fires during a request | Event thread calls `emit` under each session's own `emit_st` lock only; no bridge-wide lock taken |
| `teardown_session` + concurrent event | `session_state` removed from `sessions_` first, so a subsequent broadcast simply won't reach it; `emit_st.active` guards an event already in flight for this session |

---

## Invariants

1. `try_handle_request()` never touches `ctx.objects` — every object-touching
   op is either fully relayed (its six ops) or falls through untouched to the
   base `server` dispatch (everything else).
2. `session_state` is only accessible through `sessions_` (protected by
   `synchronized`). No raw pointers to session state are stored elsewhere.
3. The upstream client is connected before `server::listen()` is called. If
   `connect()` fails, `start()` throws and no downstream connections are
   accepted.
4. `teardown_session()` always runs before `cleanup_context()` (from
   `on_session_destroyed`), so `OP_DESTROY_GROUP` is sent for a session's
   group while the bridge's upstream connection is still alive.
5. `context::put_object()` is the only correct way to add an object under the
   current request's group; code that writes to `context::objects` directly
   silently opts out of group membership (and therefore bridge-relayed
   cleanup) for that object.

---

## Test coverage

Tests use `memory_server_transport` / `memory_client_transport` (in-process, synchronous).

| Test | Verifies |
|------|---------|
| Connect/disconnect | No crash, upstream session created and cleaned up |
| Object proxied upstream | `upstream.session_contexts()` shows object after instantiate; zero after destroy |
| SET + GET forwarded | Value stored on upstream object, returned to downstream client |
| Method call forwarded | `CALL_FALLBACK`-independent relay; upstream method executes; result returned |
| Events routed correctly | Upstream event for client A's object reaches A's handler only (broadcast + local filtering) |
| Event for bridge-owned object | Reaches the bridge's own upstream-client handler, not dropped |
| Object IDs are routing tags, not capabilities | A GET using another client's known object ID is relayed and succeeds — documents the intentional isolation tradeoff |
| CLEAR forwarded | Subsequent SET/GET still work after OP_CLEAR |
| Session teardown (direct) | Upstream `__destruct` hooks called for both explicitly-instantiated objects on disconnect |
| Session teardown (indirect) | A method call that creates objects via `context::put_object()` (mirroring wish's UI template pattern) has *all* of them destroyed on disconnect via `OP_DESTROY_GROUP`, not just the one object the client explicitly instantiated |

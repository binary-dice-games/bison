# Bison RMI Framework Design

## 1. Purpose

This document defines a Remote Method Invocation (RMI) framework built on top of the Bison library. It allows clients to instantiate and interact with server-side Bison objects as if they were local objects, while preserving strict client isolation and transport modularity.

The framework will:

- Use `bison::dynamic` for payloads, metadata, and protocol messages.
- Use Bison class registration and object model for remote object lifecycle.
- Support pluggable transports (TCP, HTTP, pipes, in-memory, etc.).
- Be platform independent by selecting platform-specific implementations at build time, not via runtime virtual-interface hierarchies.

## 2. Namespace and Source Layout

All code is under namespace `bdg::bison::rmi`.

Source tree:

- `src/rmi/shared`: protocol, message schema, ids, errors, common utilities.
- `src/rmi/client`: client runtime, remote object proxy, threading, request dispatch.
- `src/rmi/server`: server runtime, session context, request handlers, lifecycle.
- `src/rmi/transport`: all transport implementations. Each transport is a single cross-platform file backed by libuv:
- `socket_transport.cpp` — TCP socket transport
- `tls_socket_transport.cpp` — TLS-secured TCP socket transport (see §3.4)
- `named_pipe_transport.cpp` — named pipe / Unix domain socket transport
- `memory_transport.cpp` — in-process memory transport (no I/O, no libuv)
- `stream_transport.cpp` — wraps an external `std::iostream`

`socket_transport.cpp` and `tls_socket_transport.cpp` share one small
platform-facing step: accepted connections are moved from the listener's
`uv_loop_t` to their own dedicated loop by duplicating the underlying OS
socket descriptor (libuv's `uv_accept()` requires the client handle to
already share the listener's loop, so the connection's own loop can't be
used directly). Duplicating a socket has no libuv-portable API, so
`duplicate_tcp_socket()` (hoisted into the shared `tcp_socket_util.hpp` so
both transports use one implementation) calls POSIX `dup()` directly (shared
by native Linux and MSYS2 — the only supported targets). This does not
change transport *behavior* across platforms — it is a single OS primitive
with no libuv equivalent.

Both TCP-backed transports also share `frame_parser.hpp` (the incremental
`[4-byte length][payload]` reassembly state machine, §5.3) and build on
`uv_stream_state.hpp` (the per-connection libuv loop/queue plumbing, §5.4);
`tls_socket_transport.cpp` additionally uses `tls_stream_state.hpp`, which
wraps a `uv_stream_state` by composition to insert the mbedTLS
encrypt/decrypt step between raw socket bytes and the shared frame parser
(see §3.4 for the dependency and §13 for the security model).

## 3. Platform Independence Strategy

### 3.1 Goal

All transport I/O is implemented using **libuv** (`extern/libuv`, static target `uv_a`). libuv provides a uniform async I/O API across Linux (epoll) and Windows (IOCP) — the latter still relevant because MSYS2 builds against the mingw/Windows toolchain — eliminating any need for platform-specific transport code or `#ifdef` guards in transport sources.

### 3.2 Approach

- Every transport file is a single `.cpp` file with no platform conditionals in I/O behavior.
- libuv handles OS differences transparently: process spawning, TTY raw mode, and socket I/O.
- The only permitted `#ifdef` in transport code is for narrow, single-line differences, not a behavioral split.
- `socket_transport.cpp`'s socket-duplication step (see §2) is implemented directly with POSIX `dup()`, since both supported targets (Linux and MSYS2) share that API; every other transport needs no platform-specific code at all because libuv covers the rest of their I/O.

### 3.3 libuv Dependency

libuv is included as a git submodule at `extern/libuv` and built as a static library (`uv_a`). CMakeLists.txt links it into `bison`:

```cmake
add_subdirectory("extern/libuv")
target_link_libraries(bison PRIVATE uv_a)
```

libuv internally links `ws2_32` and other Windows system libraries as needed on MSYS2 builds; the bison build does not add them manually.

### 3.4 TLS Dependency

`tls_socket_transport` (§5.5) is built on **mbedTLS**, included as a git submodule at `extern/mbedtls` (pinned to the 3.6.x LTS branch) and linked as three static targets (`mbedtls`, `mbedx509`, `mbedcrypto`):

```cmake
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory("extern/mbedtls")
target_link_libraries(bison PRIVATE mbedtls mbedx509 mbedcrypto)
```

mbedTLS was chosen over OpenSSL, BoringSSL, and wolfSSL for this specific
constraint set -- must build via a plain `add_subdirectory()` like every
other `extern/` dependency (no Perl/NASM/Go toolchain requirement beyond
what libuv/gflags already need), must statically link into both the
`STATIC bison` target and the `SHARED bison_abi` target, must support
optional mutual TLS, and must carry a license consistent with the rest of
bison's permissive (MIT/BSD/Apache) dependency set:

- **OpenSSL** has no native CMake build (`Configure` is a Perl script, plus
  NASM for asm on Windows) -- it doesn't fit the `add_subdirectory()`
  precedent every other dependency here follows.
- **BoringSSL** needs Go and NASM to build, and upstream explicitly
  disclaims API/ABI stability for third-party embedders.
- **wolfSSL** is dual GPLv2/commercial-licensed; statically linking GPLv2
  code into bison's single binary would put the whole binary under GPL
  terms absent a commercial license.
- **mbedTLS** has a native `CMakeLists.txt` that drops in exactly like
  `extern/libuv`, is pure C with no extra toolchain requirements, builds
  cleanly with plain MSVC or mingw64, and is Apache-2.0 licensed. Its
  `mbedtls_ssl_conf_authmode()` covers both server-only and mutual TLS from
  one API.

`extern/mbedtls` itself carries a nested submodule at `framework/`
(`mbedtls-framework`), required unconditionally by mbedTLS 3.6.x's own
`CMakeLists.txt` even to build just the library (not only its test suite).
`git clone --recurse-submodules` (README.md's documented clone command)
already recurses into it; no extra step is needed.

## 4. Architecture Overview

The design follows a client/server model similar to Apache Thrift in separation of concerns:

- Business object management and invocation are independent from transport.
- Transport only provides framed message exchange and connection lifecycle.
- Protocol messages use a fixed envelope schema for strict validation and compact transfer.

Core layers:

1. API Layer: `Client`, `Server`, `remote::dynamic`.
2. Runtime Layer: worker threads, request tracking, session context.
3. Protocol Layer: operation ids, request/response/event framing, errors.
4. Transport Layer: connect/listen/send/recv/close abstractions.
5. Platform Layer: low-level OS primitives used by transport implementations.

## 5. Transport Abstraction

### 5.1 Transport Requirements

Both client and server transports must support:

- initialization with `bison::dynamic params`
- connection lifecycle (`connect` / `listen` / `accept` / `close`)
- framed message send/receive (`bison::buffer` frame payload)
- thread-safe send from one thread while receive loop runs on another

### 5.2 Transport Contracts (conceptual)

Client-side expected operations:

- `open(params)`
- `send(frame)`
- `receive(frame_out)` blocking or timeout-based
- `shutdown()`

Server-side expected operations:

- `start(params)`
- `accept()` returns per-client connection handle
- per-connection `send/receive/close`
- `stop()`

The concrete transport type is injected into `Client`/`Server` constructors.

### 5.3 Wire Framing

All libuv-backed transports use a simple **4-byte big-endian length-prefix** frame format:

```
[ 4 bytes: payload length (BE uint32) ][ N bytes: payload ]
```

The length field is written and read using `bison::byte_swap<uint32_t>` (from `bison_common.hpp`), which uses compiler intrinsics and is a no-op on big-endian hosts. There is no frame type byte or checksum — framing is purely delimitng.

### 5.4 Async-to-Sync Bridge Pattern

libuv is callback/event-loop based; bison's transport interface is synchronous (`receive` blocks until a frame arrives or a timeout elapses). Each libuv-backed transport bridges these two models with the following pattern:

**Background thread** runs `uv_run(&loop, UV_RUN_DEFAULT)` and owns all libuv handles.

**Server accept path** (socket/named-pipe listeners): the listener itself runs on
its own `uv_loop_t`/thread, but each *accepted* connection gets its own
independent `uv_loop_t`/thread rather than sharing the listener's loop. Since
`uv_accept()` requires its client handle to already share the listener's loop,
the connection handle can't simply be initialized on its own loop up front.
Instead, `on_new_connection` accepts into a temporary handle on the listener's
loop, duplicates the underlying OS descriptor, and attaches the duplicate to a
fresh handle on the connection's own loop (`uv_pipe_open` / `uv_tcp_open`).
`socket_transport.cpp` does this today; `named_pipe_transport.cpp` predates
this fix and has the same cross-loop `uv_accept()` mismatch — it just isn't
exercised by the (Linux-only) test suite, since it targets Windows named
pipes there.

**Receive path** (loop thread → caller thread):
- `alloc_cb` / `on_read` callbacks accumulate incoming bytes and parse 4-byte length-prefix frames incrementally.
- Each complete frame is pushed to a `bison::synchronized<std::queue<bison::buffer>>`, then `notify_one()` is called on that same `synchronized` (it owns its own condition variable, so no separate `std::condition_variable` is needed).
- `receive(frame, timeout)` calls `wait_for(timeout, pred)` directly on that `synchronized` queue.

**Send path** (caller thread → loop thread):
- `send(frame)` serializes the frame (4-byte header + payload) into a `std::vector<uint8_t>`, pushes to a send queue under a mutex, then calls `uv_async_send(&send_async)`.
- The `uv_async_t` callback in the loop thread drains the send queue and issues `uv_write()` for each pending buffer.

**Shutdown**: set a stop flag, call `uv_async_send(&stop_async)`. The stop callback closes all open handles; when all handles are closed `uv_run` returns and the background thread exits, which is then joined by the caller.

### 5.5 Available Transports

| Transport | Class(es) | Backing handle |
|---|---|---|
| In-process pipe | `pipe_client_transport`, `pipe_server_transport` | `uv_pipe_t` (named pipe, unique path per channel) |
| TCP socket | `socket_client_transport`, `socket_server_transport` | `uv_tcp_t` |
| TLS-secured TCP socket | `tls_socket_client_transport`, `tls_socket_server_transport` | `uv_tcp_t` + mbedTLS session (see §3.4) |
| Named pipe / Unix socket | `named_pipe_client_transport`, `named_pipe_server_transport` | `uv_pipe_t` |
| In-memory | `memory_server_transport`, `memory_client_transport` | No I/O — shared queue |
| Stream | `stream_client_transport` | Wraps external `std::iostream` |

## 6. Protocol Specification

### 6.1 Message Envelope

Every frame is a `bison::dynamic` object with required fields:

- `__version` (int): protocol version, initially `1`
- `__kind` (hash/key): hashed token for `request`, `response`, or `event`
- `__op` (hash/key): hashed operation token for request/event
- `__requestId` (string/opaque id): correlation id (required for request/response)
- `__objectId` (string/opaque id): remote object id when applicable
- `__oneway` (bool): request indicates no response expected
- `__payload` (dynamic/blob carrier): operation-specific content
- `__error` (dynamic/blob carrier|null): error object for responses on failure

Token compaction rule:

- For fields with a small bounded vocabulary, protocol uses hashed tokens
  (`key_t`/`hash_t`) instead of raw strings to reduce frame size.
- Canonical source names remain stable protocol constants and are hashed with
  the same key function on both peers.
- This applies at least to `kind`, `op`, and error `code`.
- Identifier-like values should default to hashed keys whenever possible
  (class ids, operation ids, event names, error codes, and similar ids).
- Free-form or high-cardinality content stays as string or regular dynamic
  fields.
- By convention, all protocol-defined internal keys use the `__` prefix to
  avoid collisions with user-defined fields.

Envelope encoding rule:

- Envelope MUST be serialized/deserialized with `serializeWithSchema` and
  `deserializeWithSchema`.
- The envelope class schema is fixed and registered on both client and server.
- Unknown or missing required envelope fields are treated as protocol
  validation errors.

Payload encoding rule:

- The `payload` field contains operation data serialized with regular
  self-describing serialization (`serialize` / `deserialize`).
- This keeps payloads flexible because remote objects may have dynamic,
  per-instance fields that are not fixed ahead of time.

### 6.2 Operations

Supported request operation names (hashed in wire envelope):

- `connect`
- `describe`
- `instantiate`
- `clear`
- `set`
- `get`
- `call`
- `destroy`
- `disconnect`

Supported event operation:

- `event` (server -> client callback dispatch)

Recommended constant pattern:

- Define protocol constants once in shared code, for example
  `KIND_REQUEST = "request"_key`, `KIND_RESPONSE = "response"_key`,
  `KIND_EVENT = "event"_key`, and equivalent constants for each operation.
- Compare hashed constants directly during dispatch.

### 6.2.1 Shared Hashed Constants

The protocol should define all reserved identifiers in shared code as hashed
constants so client and server use the exact same tokens.

Recommended constants:

```cpp
namespace bdg::bison::rmi::shared::constants {

inline constexpr bison::key_t KIND_REQUEST  = "request"_key;
inline constexpr bison::key_t KIND_RESPONSE = "response"_key;
inline constexpr bison::key_t KIND_EVENT    = "event"_key;

inline constexpr bison::key_t OP_CONNECT     = "connect"_key;
inline constexpr bison::key_t OP_DESCRIBE    = "describe"_key;
inline constexpr bison::key_t OP_INSTANTIATE = "instantiate"_key;
inline constexpr bison::key_t OP_CLEAR       = "clear"_key;
inline constexpr bison::key_t OP_SET         = "set"_key;
inline constexpr bison::key_t OP_GET         = "get"_key;
inline constexpr bison::key_t OP_CALL        = "call"_key;
inline constexpr bison::key_t OP_DESTROY     = "destroy"_key;
inline constexpr bison::key_t OP_DISCONNECT  = "disconnect"_key;
inline constexpr bison::key_t OP_EVENT       = "event"_key;

inline constexpr bison::key_t HOOK_CONSTRUCT = "__construct"_key;
inline constexpr bison::key_t HOOK_DESTRUCT  = "__destruct"_key;
inline constexpr bison::key_t HOOK_CLEAR     = "__clear"_key;
inline constexpr bison::key_t HOOK_SETTER    = "__setter"_key;
inline constexpr bison::key_t HOOK_GETTER    = "__getter"_key;

inline constexpr bison::key_t ERR_INVALID_REQUEST    = "INVALID_REQUEST"_key;
inline constexpr bison::key_t ERR_UNSUPPORTED_VERSION = "UNSUPPORTED_VERSION"_key;
inline constexpr bison::key_t ERR_UNKNOWN_OPERATION   = "UNKNOWN_OPERATION"_key;
inline constexpr bison::key_t ERR_CLASS_NOT_FOUND     = "CLASS_NOT_FOUND"_key;
inline constexpr bison::key_t ERR_OBJECT_NOT_FOUND    = "OBJECT_NOT_FOUND"_key;
inline constexpr bison::key_t ERR_ACCESS_DENIED       = "ACCESS_DENIED"_key;
inline constexpr bison::key_t ERR_VALIDATION_ERROR    = "VALIDATION_ERROR"_key;
inline constexpr bison::key_t ERR_INTERNAL_ERROR      = "INTERNAL_ERROR"_key;
inline constexpr bison::key_t ERR_TIMEOUT             = "TIMEOUT"_key;
inline constexpr bison::key_t ERR_TRANSPORT_ERROR     = "TRANSPORT_ERROR"_key;

} // namespace bdg::bison::rmi::shared::constants
```

Rules:

- These constants are normative for the wire protocol.
- Client and server must not duplicate string literals ad hoc in dispatch code.
- Reserved hook names and protocol operation names should always be compared by hashed key.

### 6.2.2 Envelope Template Schema

The protocol envelope should be implemented as a registered Bison class with a
fixed field layout so `serializeWithSchema` and `deserializeWithSchema`
can be used deterministically on both peers.

Recommended shared envelope class:

```cpp
namespace bdg::bison::rmi::shared::constants {

inline constexpr bison::key_t CLASS_ENVELOPE = "__envelope"_key;

inline constexpr bison::key_t FIELD_VERSION    = "__version"_key;
inline constexpr bison::key_t FIELD_KIND       = "__kind"_key;
inline constexpr bison::key_t FIELD_OP         = "__op"_key;
inline constexpr bison::key_t FIELD_REQUEST_ID = "__requestId"_key;
inline constexpr bison::key_t FIELD_OBJECT_ID  = "__objectId"_key;
inline constexpr bison::key_t FIELD_ONEWAY     = "__oneway"_key;
inline constexpr bison::key_t FIELD_PAYLOAD    = "__payload"_key;
inline constexpr bison::key_t FIELD_ERROR      = "__error"_key;

} // namespace bdg::bison::rmi::shared::constants
```

Recommended envelope prototype:

```cpp
auto envelope = bison::dynamic_ptr{
  bdg::bison::rmi::shared::constants::CLASS_ENVELOPE,
  {
    {bdg::bison::rmi::shared::constants::FIELD_VERSION, int32_t{1}},
    {bdg::bison::rmi::shared::constants::FIELD_KIND, bison::key_t{0}},
    {bdg::bison::rmi::shared::constants::FIELD_OP, bison::key_t{0}},
    {bdg::bison::rmi::shared::constants::FIELD_REQUEST_ID, std::string{}},
    {bdg::bison::rmi::shared::constants::FIELD_OBJECT_ID, std::string{}},
    {bdg::bison::rmi::shared::constants::FIELD_ONEWAY, false},
    {bdg::bison::rmi::shared::constants::FIELD_PAYLOAD, std::string{}},
    {bdg::bison::rmi::shared::constants::FIELD_ERROR, std::string{}}
  }
};
```

Field meaning:

- `__version`: protocol version number.
- `__kind`: hashed message kind token.
- `__op`: hashed operation token.
- `__requestId`: opaque request correlation identifier serialized as a string.
- `__objectId`: opaque remote object identifier serialized as a string when applicable.
- `__oneway`: indicates whether a response is expected.
- `__payload`: serialized bytes of the regular self-describing payload, carried as a binary/string blob.
- `__error`: serialized bytes of the regular self-describing error object, carried as a binary/string blob when applicable.

Schema rules:

- The envelope class must be registered identically on client and server before any protocol messages are exchanged.
- Because Bison template serialization relies on stable field order and field types, envelope field types must not vary across operations.
- Optional logical fields are still physically present in the template and use neutral defaults when not applicable.
- `__requestId`, `__objectId`, `__payload`, and `__error` are encoded as opaque string/buffer fields inside the template envelope to avoid mixing template serialization with nested variable-shape template content.

Encoding sequence:

1. Build regular self-describing payload object for the operation.
2. Serialize payload to bytes.
3. Populate envelope fields, storing payload bytes in `FIELD_PAYLOAD`.
4. Serialize envelope with `serializeWithSchema`.

Decoding sequence:

1. Deserialize envelope with `deserializeWithSchema`.
2. Validate fixed envelope fields.
3. Deserialize `FIELD_PAYLOAD` bytes with regular deserialization when payload is present.
4. Deserialize `FIELD_ERROR` bytes similarly when error is present.

### 6.3 Error Model

Error response schema (`__error` field after decoding):

- `__code` (hash/key): hashed canonical error token
- `__message` (string)
- `__details` (dynamic, optional)

Initial canonical codes:

- `INVALID_REQUEST`
- `UNSUPPORTED_VERSION`
- `UNKNOWN_OPERATION`
- `CLASS_NOT_FOUND`
- `OBJECT_NOT_FOUND`
- `ACCESS_DENIED`
- `VALIDATION_ERROR`
- `INTERNAL_ERROR`
- `TIMEOUT`
- `TRANSPORT_ERROR`

Error token rule:

- The canonical names above are specification names.
- Wire encoding uses their hashed form in `__error.__code`.

### 6.4 Oneway Semantics

If `oneway == true`, server executes request but does not send response frame. Client resolves the returned future immediately with an empty dynamic result.

### 6.5 Serialization Policy and Validation

The wire protocol uses a hybrid strategy:

- Protocol envelope: template serialization for strict schema validation,
  deterministic field order, and lighter frame overhead.
- Operation payload: standard self-describing serialization to preserve
  dynamic object field variability.

Validation flow:

1. Decode envelope with template deserialization.
2. Validate required envelope fields (`__version`, `__kind`, `__op`, `__requestId`
   when applicable).
3. Decode payload with regular deserialization.
4. Validate operation-specific payload semantics in handler layer.

Dispatch and validation must use hashed token comparisons for bounded fields
instead of string comparisons.

Failure handling:

- Envelope template decode failure or required-field mismatch returns
  `INVALID_REQUEST`.
- Unsupported envelope version returns `UNSUPPORTED_VERSION`.
- Payload semantic issues return `VALIDATION_ERROR`.

## 7. Public API Design

### 7.1 Client

```cpp
namespace bdg::bison::rmi {

class client {
public:
    template <typename TTransport>
    explicit client(TTransport&& transport);

    void connect(bison::dynamic&& params);
    bison::dynamic describe(bison::key_t klass = 0U);

    remote::dynamic instantiate(
        bison::key_t ns,
        bison::key_t klass,
        bison::dynamic&& params = bison::dynamic::object()
    );

    void destroy(remote::dynamic&& obj);
    void disconnect();
};

} // namespace bdg::bison::rmi
```

Behavior:

- `connect` initializes transport and starts a worker thread.
- `describe(0U)` returns all server-registered classes.
- `describe(klass)` returns full metadata for the specific class, including fields, methods, and events.
- `instantiate` receives a namespace id and hashed class id, creates a server object in current session context, and returns proxy.
- `destroy` consumes a remote object proxy and releases the corresponding server object in current session context.
- `disconnect` gracefully tears down worker thread and connection.

Ownership rule:

- `instantiate` returns the unique owning proxy for the remote object.
- The proxy may be moved but not copied.
- `destroy` consumes the proxy so the API prevents post-destroy use by construction.

### 7.2 remote::dynamic Proxy

```cpp
namespace bdg::bison::rmi::remote {

class dynamic {
public:
  dynamic(const dynamic&) = delete;
  dynamic& operator=(const dynamic&) = delete;
  dynamic(dynamic&&) noexcept;
  dynamic& operator=(dynamic&&) noexcept;

    void clear();
    void set(bison::dynamic&& fields);
    void get(bison::dynamic& fields);

    std::future<bison::dynamic> call(
        bison::dynamic&& params,
        bool oneway = false
    );

    void onEvent(
      bison::key_t name,
        std::function<void(bison::dynamic&& params)> handler
    );

    uint64_t id() const;
};

} // namespace bdg::bison::rmi::remote
```

Behavior details:

- `remote::dynamic` is move-only and non-copyable.
- A remote object has exactly one owning proxy instance on the client side.
- Ownership must be transferred explicitly with move semantics when passing the proxy between scopes or components.
- A moved-from proxy is invalid for further operations except implementation-defined destruction-safe cleanup behavior.
- `clear`: clears explicitly set fields and reverts to inherited/default model behavior.
- `set`: applies partial update to provided fields only.
- `get`:
  - if input `fields` is empty, returns full object snapshot.
  - if `fields` is a projection shape, fills only requested members (GraphQL-like projection).
- `call`: executes remote callable behavior and returns future.
- `onEvent`: registers a handler for a hashed event id; callback is dispatched on the client's single worker thread.
- Unique proxy ownership prevents two live local proxies from independently destroying the same remote object.

### 7.3 Server

```cpp
namespace bdg::bison::rmi {

class server {
public:
    template <typename TTransport>
    explicit server(TTransport&& transport);

    void listen(bison::dynamic&& params);
    void stop();
};

} // namespace bdg::bison::rmi
```

Behavior:

- `listen` starts accept loop thread and initializes transport.
- each accepted client receives isolated `context`.
- each client is processed by dedicated worker thread.
- `stop` terminates accept loop, closes active connections, and joins workers.

## 8. Server Context and Isolation

Each client connection owns a unique `context` object.

`context` contains at minimum:

- `sessionId`
- `objects: unordered_map<uint64_t, bison::dynamic>` (or owning wrapper)
- random object id generator / collision-resistant id source
- `subscriptions` (event handlers metadata if needed server-side)
- request bookkeeping / cancellation flags

Isolation guarantees:

- object ids are scoped to the session context.
- client A cannot access objects created by client B.
- on disconnect, all objects in that context are destroyed, and `__destruct` is invoked for each remaining object if registered.
- within one client session, the API is designed so one live remote object maps to one live owning proxy.

## 9. Threading and Concurrency Model

### 9.1 Client Side

- Main/user thread invokes API methods.
- A single worker thread per client owns the receive loop and event dispatch.
- Outbound requests are serialized through a thread-safe send path.
- Pending request map: `requestId -> promise` protected by mutex.
- Event callbacks are executed serially on that worker thread to preserve ordering.

Flow:

1. API method creates request with new `requestId`.
2. Promise/future pair registered in pending map (unless oneway).
3. Frame sent over transport.
4. Worker receives response/event and dispatches:
   - response -> resolve/reject promise.
  - event -> invoke the registered handler on the same client worker thread.

### 9.2 Server Side

- One listening thread handles `accept` loop.
- One worker thread per active client connection handles request loop.
- Per-client context is only mutated by that client worker thread (preferred) to reduce lock contention.
- Shared registries (class metadata) are read-only after startup or protected by RW lock.

## 10. Operation Semantics

### 10.1 connect

- Client sends optional params for auth/session metadata: `client::connect(dynamic params)` forwards the same `params` to both the transport's `open()` and this operation's payload.
- Server validates protocol version and returns server capabilities.
- `payload` in this operation is regular self-describing dynamic data.
- Authentication is an optional server-side hook, not a protocol-level concept: `server::listen()` accepts an `auth_module_ptr` (see `src/rmi/server/auth.hpp`); when set, `handle_connect()` calls its `authenticate(ctx, payload, out_identity)` before acking, and rejects with `ERR_ACCESS_DENIED` on failure. The RMI envelope/wire format itself defines no auth-specific fields -- `auth_module_iface` reads whatever it needs out of the ordinary connect payload. Transport-level security (TLS, local pipe ACLs, transport credentials) remains a separate, transport-specific concern.

### 10.2 describe

- Request payload: `{ "__class": 0 | <key/hash> }`
- Response payload:
  - all classes: list/map of class descriptors.
  - single class: full metadata including field schema, inheritance, methods, and events.
- Request and response payloads use regular self-describing serialization.
- A zero class id in `__class` requests the full class list; a non-zero hashed class id requests metadata for that specific class.

### 10.3 instantiate

- Request payload: `{ "__class": <key/hash>, "__params": {...} }`
- Server creates object using registered Bison class factory and binds to session context.
- If the instantiated object registers a `__construct` method, the server invokes it immediately after object creation, passing the instantiate `params` as call arguments.
- Response payload: `{ "__objectId": <opaque-random-id>, "__class": <key/hash> }`
- Params and response payload use regular self-describing serialization.
- Class id field `__class` MUST be transmitted as hashed key/token, not full class string.
- If `__construct` fails, object creation is considered failed and the object is not retained in the session context.
- Object ids MUST be opaque random identifiers rather than incremental counters to reduce guessing attacks across the protocol boundary.

### 10.4 clear

- Clears explicitly assigned fields from remote object.
- The framework first clears the explicitly assigned fields from the remote object.
- If the object defines `__clear`, the server invokes it after the clear has been applied.
- `__clear` allows post-clear setup, reinitialization of derived state, and any object-specific logic that must run after the object has been reset.
- Operation payload (if present) uses regular self-describing serialization.

### 10.5 set

- Applies partial field updates without resetting unspecified fields.
- If the object defines `__setter`, the incoming set payload is first passed to `__setter` and the returned payload is used as the effective field patch.
- `__setter` allows validation, normalization, derived-field updates, and custom write-side transformation before fields are applied.
- Field patch payload uses regular self-describing serialization.

### 10.6 get

- Empty projection returns full object snapshot.
- Non-empty projection returns only selected sub-tree fields.
- After the server computes the get result, if the object defines `__getter`, the result payload is passed through `__getter` before being sent back to the client.
- `__getter` allows filtering, projection adjustment, computed fields, and read-side transformation.
- Projection request and snapshot response payloads use regular
  self-describing serialization.

### 10.7 call

- Invokes object callable behavior with params payload.
- Returns call result in response payload, unless oneway.
- Call params and result payloads use regular self-describing serialization.

### 10.8 destroy

- Client passes `remote::dynamic&&`; implementation extracts the proxy object id and sends destroy request for that id.
- Before releasing the remote object, if the object defines `__destruct`, the server invokes it.
- Server removes object id from context and releases associated resources.
- Operation payload (if present) uses regular self-describing serialization.
- After `destroy`, the consumed proxy is no longer valid, and no second live owning proxy should exist for that object.
- If `__destruct` fails, the server should still complete object cleanup on a best-effort basis and report the failure according to destroy error policy.

### 10.10 Object Hook Methods

The server recognizes a small set of reserved object methods that customize remote object lifecycle and field access behavior:

- `__construct`: invoked on instantiate, with instantiate params.
- `__destruct`: invoked on destroy and disconnect-triggered cleanup, before final object release.
- `__clear`: invoked during clear.
- `__setter`: invoked before applying a set payload.
- `__getter`: invoked after computing a get result and before returning it.

Hook rules:

- These are ordinary Bison methods registered on the object, but their names are reserved by the RMI framework.
- They should be referenced internally using hashed method keys.
- Hooks are optional; if absent, default framework behavior is used.
- Hook execution occurs on the server-side client worker thread for that session.
- Hook failures are surfaced as operation failures unless the framework explicitly specifies best-effort cleanup behavior, as in destroy.

### 10.9 disconnect

- Client requests graceful closure.
- Server invokes `__destruct` for each remaining live object that defines it, then destroys context objects and closes connection.
- Disconnect-triggered cleanup is best-effort: failures in `__destruct` should be logged or surfaced according to disconnect error policy, but must not prevent release of remaining session objects.
- Operation payload (if present) uses regular self-describing serialization.

## 11. Eventing Model

Events are server-initiated frames with `kind = "event"`.

Event payload:

- `__objectId`
- `__name` (event id as hash/key)
- `__params` (dynamic)

Event transport rule:

- Event envelope uses template serialization.
- Event params use regular self-describing serialization.
- Event kind/op token matching uses hashed constants.

Client dispatch rules:

- match `(__objectId, __name)` handler if registered.
- execute handler on the client worker thread.
- handler exceptions are caught and logged; they must not crash worker loop.
- Event callback ordering is guaranteed per client because handlers run serially on a single worker thread.

## 12. Reliability and Timeouts

Recommended defaults:

- request timeout configurable per client instance.
- transport read timeout optional; reconnect policy transport-specific.
- on disconnection, all pending promises fail with `TRANSPORT_ERROR`.
- if a client disconnects without explicit `destroy`, server-side session cleanup invokes `__destruct` for each remaining live object when registered, then releases all remaining uniquely-owned remote objects in that session.
- oneway calls complete their returned futures immediately on the client without waiting for server acknowledgment.

## 13. Security and Privacy Baseline

- Session-scoped object visibility is mandatory.
- Server validates all object ids against session context.
- Remote object ids are opaque random values and must not be predictable from previous allocations.
- Input payload validation is required before invoking class methods.
- Transport-level security and authentication (TLS, local pipe ACL, transport credentials, etc.) are delegated to transport implementations. `tls_socket_transport` (§3.4, §5.5) provides this for TCP: confidentiality plus mandatory server authentication via a certificate (like HTTPS), with optional mutual TLS (client certificates, opt-in per listener via `client_auth`) for deployments that want it. mTLS defaults to off because `auth_module_iface`'s shape (a per-connection, app-supplied credential check keyed off the `"connect"` payload -- see §10.1) already covers client identity for the common case of an open/product-facing client population; mTLS is a machine-identity mechanism better suited to a small, fixed fleet (service-to-service/zero-trust), where the cost of issuing and rotating a certificate per client is justified. The two compose without any interface change: mTLS (when enabled) authenticates the TCP connection itself before any bison bytes flow; `auth_module_iface` then authenticates the session on top, over the resulting confidential channel -- `auth.hpp`/`server.hpp` need no TLS-awareness at all.

## 14. Extensibility

Extension points:

- New transport modules implementing send/receive contract.
- New protocol operations with versioned capability negotiation.
- Optional middleware hooks (logging, auth, metrics, tracing).

Versioning strategy:

- Envelope `version` field with backward-compatible parsing.
- Server advertises supported versions/capabilities during `connect`.
- Envelope schema changes require explicit version bump and template
  compatibility checks on both peers.

## 15. Implementation Status

**Completed:**

- Shared envelope schema, operation constants, and hashed protocol ids
- Client runtime: connect, describe, instantiate, set, get, call, destroy, disconnect
- Server runtime: listen, accept loop, per-session context isolation, request dispatch, all hook methods
- Auth hook: `auth_module_iface` + `server::listen()`'s `auth_module` parameter + `on_authenticated()` hook (see § 10.1) -- policy-free accept/reject and optional identity extraction, evaluated once per connection from `handle_connect()`
- Event dispatch (`onEvent`, server-initiated event frames)
- All transport implementations (in-memory, TCP, TLS-secured TCP, named pipe, anonymous pipe) — all cross-platform via libuv
- C ABI (`rmi_c.h`) exposing client, server lifecycle to C and language bindings
- CLI wiring for `tls_socket_transport`: `--transport=tls` on `server_app`/`client_app` (`bison-cli`, `calc-server`, `rmi_server_example`, `rmi_client_example`) and `--downstream_transport=tls`/`--upstream_transport=tls` on `bridge_app` (`rmi_bridge_example`), including the new `server_app::on_listen_params()`/`bridge_app::on_downstream_listen_params()` hooks that inject cert/key material into `listen()`/`start()` -- see `docs/tls.md`'s "CLI usage" section

**Future:**

- HTTP transport
- C ABI/binding wiring for `tls_socket_transport` (`include/rmi_c.h`, `bindings/`) -- the C++ transport and its CLI/`bridge_app` wiring are complete, see §3.4/§5.5/§13 and `docs/tls.md`
- Capability negotiation in the RMI protocol layer (auth hooks are implemented -- see § 10.1 and "Completed" above)
- Performance optimization and batching

## 16. Testing Strategy

Unit tests:

- protocol encode/decode validation
- request correlation and oneway behavior
- projection-based `get` semantics
- context isolation between multiple simulated clients

Integration tests:

- in-memory end-to-end client/server operations
- transport-specific connect/disconnect edge cases
- server cleanup of context objects on abrupt disconnect

Concurrency tests:

- many in-flight requests and ordered response matching
- concurrent sessions with object id overlap safety

## 17. Resolved Decisions

- `call(oneway=true)` resolves its returned future immediately with an empty dynamic result.
- Each client uses a single worker thread for inbound event dispatch, guaranteeing callback ordering.
- Authentication remains transport-specific in v1.
- Remote object ids are opaque random values, not incremental counters.

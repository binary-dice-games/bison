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
- `src/rmi/platform/windows`, `src/rmi/platform/linux`, `src/rmi/platform/macos`: platform-specific primitives (socket wrappers, eventing, thread naming, OS handles, etc.).

## 3. Build and Platform Independence Strategy

### 3.1 Goal

Platform-dependent code is isolated in platform folders and compiled conditionally. Shared/client/server logic remains platform agnostic.

### 3.2 Approach

- CMake selects exactly one platform folder according to target OS.
- Shared/client/server code includes platform adapters through platform headers that resolve to selected implementation.
- No platform polymorphism through virtual interfaces is required.

### 3.3 CMake Pattern

- `target_sources(bison PRIVATE ...)` includes:
  - Always: `src/rmi/shared/**`, `src/rmi/client/**`, `src/rmi/server/**`
  - Conditionally: one of `src/rmi/platform/windows/**`, `src/rmi/platform/linux/**`, `src/rmi/platform/macos/**`

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
- framed message send/receive (`bison::dynamic` frame payload)
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

## 6. Protocol Specification

### 6.1 Message Envelope

Every frame is a `bison::dynamic` object with required fields:

- `version` (int): protocol version, initially `1`
- `kind` (hash/key): hashed token for `request`, `response`, or `event`
- `op` (hash/key): hashed operation token for request/event
- `requestId` (uint64): correlation id (required for request/response)
- `objectId` (uint64): remote object id when applicable
- `oneway` (bool): request indicates no response expected
- `payload` (dynamic): operation-specific content
- `error` (dynamic|null): error object for responses on failure

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

Envelope encoding rule:

- Envelope MUST be serialized/deserialized with `serializeWithTemplate` and
  `deserializeWithTemplate`.
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

### 6.3 Error Model

Error response schema (`error` field):

- `code` (hash/key): hashed canonical error token
- `message` (string)
- `details` (dynamic, optional)

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
- Wire encoding uses their hashed form in `error.code`.

### 6.4 Oneway Semantics

If `oneway == true`, server executes request but does not send response frame. Client completes the returned future immediately with an empty dynamic result (or dedicated `void` marker payload).

### 6.5 Serialization Policy and Validation

The wire protocol uses a hybrid strategy:

- Protocol envelope: template serialization for strict schema validation,
  deterministic field order, and lighter frame overhead.
- Operation payload: standard self-describing serialization to preserve
  dynamic object field variability.

Validation flow:

1. Decode envelope with template deserialization.
2. Validate required envelope fields (`version`, `kind`, `op`, `requestId`
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
    bison::dynamic describe(const std::string& name = "");

    remote::dynamic instantiate(
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
- `describe("")` returns all server-registered classes.
- `describe(name)` returns schema/fields for specific class.
- `instantiate` receives a hashed class id, creates server object in current session context, and returns proxy.
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
        const std::string& name,
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
- `onEvent`: registers per-object callback; callback executes on client worker thread.
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
- `nextObjectId`
- `subscriptions` (event handlers metadata if needed server-side)
- request bookkeeping / cancellation flags

Isolation guarantees:

- object ids are scoped to the session context.
- client A cannot access objects created by client B.
- on disconnect, all objects in that context are destroyed.
- within one client session, the API is designed so one live remote object maps to one live owning proxy.

## 9. Threading and Concurrency Model

### 9.1 Client Side

- Main/user thread invokes API methods.
- Worker thread owns receive loop and event dispatch.
- Outbound requests are serialized through a thread-safe send path.
- Pending request map: `requestId -> promise` protected by mutex.

Flow:

1. API method creates request with new `requestId`.
2. Promise/future pair registered in pending map (unless oneway).
3. Frame sent over transport.
4. Worker receives response/event and dispatches:
   - response -> resolve/reject promise.
   - event -> invoke registered handler on worker thread.

### 9.2 Server Side

- One listening thread handles `accept` loop.
- One worker thread per active client connection handles request loop.
- Per-client context is only mutated by that client worker thread (preferred) to reduce lock contention.
- Shared registries (class metadata) are read-only after startup or protected by RW lock.

## 10. Operation Semantics

### 10.1 connect

- Client sends optional params for auth/session metadata.
- Server validates protocol version and returns server capabilities.
- `payload` in this operation is regular self-describing dynamic data.

### 10.2 describe

- Request payload: `{ "name": "" | "ClassName" }`
- Response payload:
  - all classes: list/map of class descriptors.
  - single class: field schema, inheritance, callable signature metadata.
- Request and response payloads use regular self-describing serialization.

### 10.3 instantiate

- Request payload: `{ "klass": <key/hash>, "params": {...} }`
- Server creates object using registered Bison class factory and binds to session context.
- Response payload: `{ "objectId": <id>, "klass": <key/hash> }`
- Params and response payload use regular self-describing serialization.
- Class id field `klass` MUST be transmitted as hashed key/token, not full class string.

### 10.4 clear

- Clears explicitly assigned fields from remote object.
- Operation payload (if present) uses regular self-describing serialization.

### 10.5 set

- Applies partial field updates without resetting unspecified fields.
- Field patch payload uses regular self-describing serialization.

### 10.6 get

- Empty projection returns full object snapshot.
- Non-empty projection returns only selected sub-tree fields.
- Projection request and snapshot response payloads use regular
  self-describing serialization.

### 10.7 call

- Invokes object callable behavior with params payload.
- Returns call result in response payload, unless oneway.
- Call params and result payloads use regular self-describing serialization.

### 10.8 destroy

- Client passes `remote::dynamic&&`; implementation extracts the proxy object id and sends destroy request for that id.
- Server removes object id from context and releases associated resources.
- Operation payload (if present) uses regular self-describing serialization.
- After `destroy`, the consumed proxy is no longer valid, and no second live owning proxy should exist for that object.

### 10.9 disconnect

- Client requests graceful closure.
- Server destroys context objects and closes connection.
- Operation payload (if present) uses regular self-describing serialization.

## 11. Eventing Model

Events are server-initiated frames with `kind = "event"`.

Event payload:

- `objectId`
- `name` (event name)
- `params` (dynamic)

Event transport rule:

- Event envelope uses template serialization.
- Event params use regular self-describing serialization.
- Event kind/op token matching uses hashed constants.

Client dispatch rules:

- match `(objectId, name)` handler if registered.
- execute handler on client worker thread.
- handler exceptions are caught and logged; they must not crash worker loop.

## 12. Reliability and Timeouts

Recommended defaults:

- request timeout configurable per client instance.
- transport read timeout optional; reconnect policy transport-specific.
- on disconnection, all pending promises fail with `TRANSPORT_ERROR`.
- if a client disconnects without explicit `destroy`, server-side session cleanup releases all remaining uniquely-owned remote objects in that session.

## 13. Security and Privacy Baseline

- Session-scoped object visibility is mandatory.
- Server validates all object ids against session context.
- Input payload validation is required before invoking class methods.
- Transport-level security (TLS, local pipe ACL, etc.) is delegated to transport implementations.

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

## 15. Initial Implementation Plan

Phase 1 (MVP):

- shared envelope and operation constants
- client/server skeletons with connect/listen/stop
- instantiate/set/get/call/destroy
- per-session context isolation
- in-memory transport for deterministic unit tests

Phase 2:

- describe metadata richness
- event dispatch (`onEvent`)
- timeout/error hardening
- TCP transport

Phase 3:

- HTTP/pipe transports
- auth hooks and capability negotiation
- performance optimization and batching

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

## 17. Open Clarifications

The following points need your confirmation before implementation starts:

1. Should `describe(class)` return only fields, or fields + methods + events metadata?
2. For `call(oneway=true)`, should the client future resolve immediately with empty payload, or return a special lightweight completed future type?
3. Should event callbacks support optional dispatch to a user-provided executor/thread pool, or stay fixed to the client worker thread for v1?
4. Do we need built-in authentication in v1 `connect` flow, or keep auth fully transport-specific?
5. Should object ids be simple session-local incremental `uint64_t`, or opaque random ids?

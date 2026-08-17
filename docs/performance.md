# Performance Benchmark and Optimizations

## Benchmark Architecture

The benchmark in `examples/performance.cpp` compares plain C++ struct, `bison::dynamic`, and `nlohmann::json` across nine operations:

- **create / destroy** — object construction and destruction
- **field set / get** — reading and writing named fields
- **method-style calls** — invoking registered methods
- **serialize** / **serialize (buf)** — encoding to binary, stream- and buffer-based (reuses prebuilt objects to isolate serialization cost)
- **deserialize** / **deserialize (buf)** — decoding from binary, stream- and buffer-based (reuses prebuilt payloads)
- **copy / clone** — deep-copying a prebuilt object (`dynamic::clone()`, `NativeRecord`'s copy constructor, `json`'s copy constructor)
- **nested build/read** — constructing an object with one nested child object and reading a field back through it (`dynamic_ptr` field vs. a nested native struct vs. a nested `json` object)
- **schema serialize** / **schema deserialize** — the compact, key-less wire format (`dynamic::serializeWithSchema()` / `deserializeWithSchema()`, requiring a matching `addClass` registration); these two rows reuse the plain serialize/deserialize rows' C++/json baselines, since those already measure the same binary/text encodings at the same abstraction level — only the `dynamic` column differs.

Each row reports `min/median` milliseconds. `dyn x` and `json x` ratio columns use median times to reduce outlier sensitivity. A volatile sink prevents dead-code elimination.

See [examples.md](examples.md) for build and run instructions.

---

## Implemented Optimizations

### Compiler-intrinsic byte swap

`byte_swap` uses `__builtin_bswap16/32/64` on GCC/Clang — lowers to a single `bswap` (x86) or `rev` (ARM) instruction instead of a loop.

### Buffer serializer / deserializer

`buffer_serializer` and `buffer_deserializer` read/write directly from `bdg::bison::buffer` (`std::vector<uint8_t>`, or `const uint8_t*` + length), eliminating virtual dispatch overhead of `std::ostream` / `std::istream`.

```cpp
// Serialize to buffer:
buffer_serializer out;
obj.serialize(out);
bdg::bison::buffer bytes = out.release();

// Deserialize from buffer:
buffer_deserializer in(bytes);
auto copy = dynamic::deserialize(in);
```

All `serialize` / `deserialize` overloads and `serializeWithSchema` / `deserializeWithSchema` have buffer variants. The benchmark includes `Serialize (buf)` and `Deserialize (buf)` rows comparing stream vs. buffer paths.

### `fields_` retained as `std::map`

`fields_` serves dual purpose: named-field dictionary (keys with MSB set) and array (small numeric keys 0, 1, 2, …). `std::map` guarantees ascending key order, which is required for:

- **Array semantics** — numeric indices must be iterated in order for `size()` and field iteration to be correct.
- **Template serialization** — `serializeWithSchema` / `deserializeWithSchema` must write and read fields in identical prototype-chain order. `std::unordered_map` has non-deterministic iteration order across process restarts.

`size()` and `clear()` use `lower_bound(0x80000000u)` to separate numeric from named keys in O(log n).

### Python and C# bindings: memoized field/method key hashing

In C++, `"name"_key` is a `constexpr` FNV-1a hash computed at compile time. Neither Python nor C# has an equivalent, so every field/method access must hash its name at run time by crossing the FFI boundary into `bison_key()` (`src/bison/bison_c.cpp`) — encoding the string and paying the marshaling cost (ctypes' per-call GIL release/reacquire in Python; a P/Invoke call in C#) each time.

Both bindings memoize that call in a bounded (4096-entry) cache, since real callers draw field/method names from a small, static, schema-defined set reused across many calls (every `Dynamic` field/method access funnels through it), so the cache turns almost all lookups after the first into a dictionary hit:

- **Python** — `bison.dynamic.key(name)`, memoized with `functools.lru_cache(maxsize=4096)`. Bounded rather than unbounded (`functools.cache`) so that callers hashing high-cardinality or externally-derived strings can't grow it without bound. See `bindings/python/bison/dynamic.py`.
- **C#** — `Bdg.Bison.Key.Of(name)`, memoized in a `ConcurrentDictionary` capped at 4096 entries (bounded rather than evicting-LRU: once full, new names just fall back to the native call instead of paying eviction bookkeeping on every insert). See `bindings/csharp/Bison/Key.cs`.

### RMI server: per-session cost reduction

A scaling analysis of the RMI server (`src/rmi/server`, `src/rmi/transport`)
identified several sources of avoidable per-connection/per-request CPU,
memory, and network cost. The following were implemented:

- **TCP socket tuning** (`src/rmi/transport/tcp_socket_util.hpp`'s
  `tune_tcp_socket()`, applied by both `socket_transport.cpp` and
  `tls_socket_transport.cpp` on every client and accepted-server connection):
  `TCP_NODELAY` (via `uv_tcp_nodelay`) disables Nagle's algorithm, which
  otherwise adds latency to bison's small, frequent request/response frames;
  `SO_KEEPALIVE` (via `uv_tcp_keepalive`) lets the OS eventually detect a peer
  that vanished without a clean FIN/RST, instead of that connection's thread
  and buffers leaking indefinitely.
- **Bounded frame allocation** (`src/rmi/transport/frame_parser.hpp`'s
  `kMaxFrameBytes`, 64 MiB): the 4-byte wire length prefix (FORMAT.md §5.1)
  is attacker/corruption-controlled and read before any payload bytes
  arrive; `frame_parser::feed()` now rejects a declared length above this
  ceiling before reserving space for it, closing the connection instead of
  attempting a multi-gigabyte allocation from a bogus header. Applied to
  every libuv-backed transport (TCP, TLS, named pipe) and to
  `stream_transport.cpp`'s iostream-based framing.
- **Cross-frame buffer reuse** (`frame_parser::offer_reuse()` +
  `uv_stream_state::recycle_slot`): each frame's payload buffer previously
  had to be freshly allocated, since ownership moves out to the receive
  queue once a frame completes. `dequeue_frame()` now hands a
  fully-consumed frame buffer's spare capacity back to the connection via a
  small synchronized slot, and the next `feed()` call reuses it instead of
  a fresh `malloc`, once a connection reaches steady-state message sizes.
- **Skipped error-payload encoding on success** (`src/rmi/shared/envelope.cpp`):
  every response previously built and schema-serialized a full `"__error"`
  object — including a class-registry read lock — even on success, where
  `error` never has more than its always-present `CLASS` field set.
  `envelope::encode()` now checks for an explicitly-set `__code` field and,
  when absent, leaves `FIELD_ERROR` unset entirely so the schema encoder
  falls back to the envelope prototype's empty-buffer default (see
  FORMAT.md §4.5). `envelope::decode()` mirrors this, skipping
  `deserializeWithSchema` for an empty `__error` buffer.
- **Avoided redundant clones on the request hot path**
  (`src/rmi/server/server.cpp`): `handle_set` previously deep-copied the
  incoming payload unconditionally, even though it's only needed as an
  owned value when a `__setter` hook transforms it — the common
  no-hook case now applies fields directly from `env.payload` with zero
  copies. `handle_get`'s full-snapshot path (no projection) previously
  called `dynamic::clone()`, which heap-allocates a `shared_ptr` control
  block via the virtual `clone_ptr()` path only to immediately unwrap and
  discard it; it now copy-constructs directly, producing an identical deep
  copy without that extra allocation.

### Core `dynamic`: shared (not duplicated) method metadata

`dynamic::methods_` (`src/bison/bison_object.hpp`) now stores
`shared_ptr<const method>` instead of `method` by value. A `method` --
including its `input_`/`output_` parameter specs -- is never mutated in
place after registration, so `findMethod()`'s inherited-method cache hit,
`dynamic`'s copy constructor, and `clone_into()` all now share the
underlying method object (a refcount bump) instead of deep-copying it
(previously including a `clone_ptr()` call on each of `input_`/`output_`).
This specifically targets many-session hosting: without it, N concurrent
sessions each instantiating the same class and calling the same M methods
would duplicate that class's method+spec storage N×M times instead of once.
See `src/bison/DESIGN.md` §5.5 for the updated inheritance-lookup design.

### RMI server: bounded dispatch worker pool

The RMI server previously spawned one dedicated OS thread per accepted
session to run its request-dispatch loop (`server::accept_loop()`), so
thread count scaled linearly with concurrent session count -- each thread
reserving its own stack, this was the dominant ceiling on sessions per
instance. `server.cpp` now services all sessions with a small **bounded**
pool of dispatch worker threads (`dispatch_worker_state`, sized off
`std::thread::hardware_concurrency()`): `accept_loop()` assigns each new
session to a worker round-robin, and each worker polls its assigned
sessions' `receive(timeout)` in turn (`kDispatchPollTimeout`, 5ms). Per-session
request ordering and locking are unchanged -- only one worker ever owns a
given session at a time -- at the cost of up to
`kDispatchPollTimeout × sessions-on-that-worker` added worst-case latency for
a session waiting behind idle sessions sharing its worker. This is
transport-agnostic (built only on the existing `server_connection_iface::receive(timeout)`
contract), so it applies uniformly to every transport without
transport-specific code. See `src/rmi/DESIGN.md` §9.2 for the design
rationale, including why a further libuv I/O-loop-level pooling pass (the
per-connection `uv_loop_t`/thread pair) was scoped out.

### Wire format: varint length prefixes

Every string/vector/Standard-Format field count was previously written as a
fixed 8-byte big-endian `size_t` (`src/bison/bison_serialization.hpp`), even
though real payloads' counts are almost always well under 2^16 -- most
strings, arrays, and field-count prefixes now cost 1 byte instead of 8.
`buffer_serializer`/`buffer_deserializer` and `stream_serializer`/
`stream_deserializer` now encode every such count as a ULEB128 varint
(`write_varint`/`read_varint`; see FORMAT.md §1.4 for the exact byte-level
format) instead. This is a **wire-breaking change** -- protocol version
bumped 1 -> 2 (`src/rmi/shared/constants.hpp`'s `PROTOCOL_VERSION`); bison
has no external readers of its wire format yet, so no compatibility shim was
added. Reduces both per-message network bytes and, since every count now
usually fits in one byte instead of eight, per-message serialization/
deserialization work.

See the RMI framework design doc (`src/rmi/DESIGN.md`) for the broader
scaling analysis this work is part of, including the still-outstanding
per-connection libuv I/O-loop/thread model (§5.4).

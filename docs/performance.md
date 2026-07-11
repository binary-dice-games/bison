# Performance Benchmark and Optimizations

## Benchmark Architecture

The benchmark in `examples/performance.cpp` compares plain C++ struct, `bison::dynamic`, and `nlohmann::json` across five operations:

- **create / destroy** — object construction and destruction
- **field set / get** — reading and writing named fields
- **method-style calls** — invoking registered methods
- **serialize** — encoding to binary (reuses prebuilt objects to isolate serialization cost)
- **deserialize** — decoding from binary (reuses prebuilt payloads)

Each row reports `min/median` milliseconds. `dyn x` and `json x` ratio columns use median times to reduce outlier sensitivity. A volatile sink prevents dead-code elimination.

See [examples.md](examples.md) for build and run instructions.

---

## Implemented Optimizations

### Compiler-intrinsic byte swap

`byte_swap` uses `__builtin_bswap16/32/64` on GCC/Clang — lowers to a single `bswap` (x86) or `rev` (ARM) instruction instead of a loop.

### Buffer serializer / deserializer

`buffer_serializer` and `buffer_deserializer` read/write directly from `std::vector<char>` (or `const char*` + length), eliminating virtual dispatch overhead of `std::ostream` / `std::istream`.

```cpp
// Serialize to buffer:
buffer_serializer out;
obj.serialize(out);
std::vector<char> bytes = out.release();

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

### Python binding: memoized field/method key hashing

In C++, `"name"_key` is a `constexpr` FNV-1a hash computed at compile time. Python has no equivalent, so `bison.dynamic.key(name)` must hash at run time by crossing the ctypes FFI boundary into `bison_key()` (`src/bison/bison_c.cpp`) on every call — encoding the string and paying ctypes' per-call GIL release/reacquire each time.

`key()` is memoized with `functools.lru_cache(maxsize=4096)`. Field and method names are drawn from a small, static, schema-defined set reused across many calls (every `Dynamic` field/method access funnels through this one function), so the cache turns almost all lookups after the first into a dict hit. The cache is bounded rather than unbounded (`functools.cache`) so that callers hashing high-cardinality or externally-derived strings can't grow it without bound. See `bindings/python/bison/dynamic.py`.

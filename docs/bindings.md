# Language Bindings

All bindings wrap the `bison_abi` shared library, which exports both the
dynamic-object API (`bison_c.h`) and the RMI API (`rmi_c.h`). Build it first:

```bash
cmake -B build -DPACKAGE_TESTS=ON
cmake --build build --config Debug --target bison_abi
```

Set `BISON_LIB` to the full path of the shared library if it is not found automatically:

```bash
# Linux:
export BISON_LIB=$(pwd)/build/libbison_abi.so
# macOS:
export BISON_LIB=$(pwd)/build/libbison_abi.dylib
# Windows:
set BISON_LIB=%cd%\build\Debug\bison_abi.dll
```

---

## C++ (header-only) (`bindings/cpp/`)

Header-only wrapper (`bindings/cpp/include/bison/`) that gives the
precompiled `bison_abi` shared library an interface mirroring the internal
`bdg::bison::dynamic` C++ API (`src/bison/bison.hpp`) as closely as the C
ABI allows — `dynamic obj{"Player"_key}; obj["hp"_key] = 100;` instead of
`bison_set_int(h, bison_key("hp"), 100)`. Unlike the Python and C#
bindings, there is no dynamic library loading step: `#include
"bison/bison.hpp"` and link `bison_abi` like any other library — the
compiler resolves `bison_c.h` / `rmi_c.h`'s C symbols at link time, the
same as an application linking any other precompiled shared library.

The binding lives in `bdg::bison::abi` (one namespace level below the
internal `bdg::bison`) rather than reusing the internal namespace verbatim.
This isn't just caution: `bison_abi`'s shared library keeps the *default*
(exported) ELF visibility on every internal C++ symbol it links in from
`libbison.a` (only the C functions declared in `bison_c.h`/`rmi_c.h` get
explicit `BISON_API` visibility), so reusing the exact name
`bdg::bison::dynamic` for a completely different, incompatible class
layout would mangle to the same symbol names `libbison_abi.so` already
exports as weak/COMDAT symbols — any consumer executable that also defines
those symbols would trigger ELF symbol interposition, silently rebinding
calls *inside* the library's own internal C++ code to this header's
unrelated class and corrupting memory. This was reproduced directly while
developing the binding; see `bindings/cpp/include/bison/dynamic.hpp`'s
top-level doc comment for the full account. `key_t`/`hash_t`/`hash()` stay
under `abi` too, for the same reason.

`"name"_key` is `constexpr`, computed by a compile-time FNV-1a hash
identical to the internal `bdg::bison::hash()` — unlike the Python/C#
bindings' `key()`/`Key.Of()`, which must call across the ABI (or, for C#,
maintain a runtime memoization cache) because those languages have no way
to hash a string literal before run time. Every `dynamic` is an RAII,
move-enabled wrapper around a `bison_handle`; failures raise
`bison_exception` / `rmi_exception` (wrapping the `bison_error` /
`rmi_error` codes) instead of requiring manual return-code checks.

A few gaps versus the internal C++ API are inherent to the ABI's surface,
not this binding's choice — see `dynamic.hpp`'s and `rmi.hpp`'s top-level
doc comments for the full list and reasoning:
- `dynamic::serialize()` / `dynamic::deserialize()` wrap `bison_serialize()`
  / `bison_deserialize()` (`bison_c.h`), the compact binary wire format
  (`FORMAT.md`) — this is the ABI-reachable equivalent of the internal
  `dynamic::serialize(buffer_serializer&)`. There is still no ABI entry
  point for `stream_serializer` (arbitrary `std::iostream` targets) or the
  schema-driven wire format (`serializeWithSchema()`); `to_json()` /
  `to_yaml()` / `pretty()` remain the only text formats, matching the
  Python/C# bindings.
- `dynamic::addMethod()`'s callback populates a `result` out-parameter in
  place rather than returning a `dynamic` by value (there is no ABI call to
  copy an arbitrary field set out of a fresh object into the library-owned
  result handle) — this is the one remaining gap without a mechanical fix,
  since it needs generic field enumeration, which `bison_c.h` doesn't
  expose at all.
- The RMI `client`/`proxy` are synchronous by default (matching
  `rmi_c.h`'s blocking calls) with `_async()` counterparts returning a
  `future` wrapping `rmi_future_handle`, rather than the internal API's
  uniform `std::future<T>` return type.

Indexed (numeric) field access (`obj[0]`) and vector-typed fields
(`obj["tags"_key] = std::vector<int32_t>{...}`) are both fully supported,
including read-back — `bison_c.h` exports `bison_{get,set}_{bool,key,
object}_at()` and `bison_{get,set}_vector_{bool,int,float,bytes}()`
alongside the scalar functions. Vector-typed fields are still named-field
only (`operator[](key_t)`), not reachable through `operator[](size_t)` —
that indexing model builds an array *out of* many scalar fields, a
different concept from a single field that itself holds a vector.

A field the C++ side declares as `bison::key_t` is a distinct field-variant
type from `int32_t` — this resolves naturally through ordinary C++ overload
resolution: `obj["id"_key] = "hero"_key;` (a `key_t` argument) picks the
`key_t` overload of `field_ref::operator=`, while `obj["id"_key] =
int32_t{7};` picks the `int32_t` overload — no separate `set_key()` call is
needed the way Python's `set_key()` / C#'s `SetKey()` need one, since C++
already knows the argument's static type. `addFieldKey()` is still the
`addField()` counterpart for schema declarations, since a bare `addField(name,
int32_t)` can't be told to register a `key_t` field instead.

**Requirements:** A `bison_abi` build (any platform), a C++20 compiler, and
the `bison_c.h` / `rmi_c.h` headers already shipped alongside it.

```bash
# Build bison_abi first (see the shared instructions above), then the
# binding's own examples/tests build as part of the normal CMake build:
cmake -B build -DPACKAGE_TESTS=ON
cmake --build build --config Debug

# Run examples:
./build/bindings/cpp/bison_cpp_example
./build/bindings/cpp/rmi_cpp_standalone_example
./build/bindings/cpp/rmi_cpp_server_example --transport=tcp --port=7070   # separate terminal
./build/bindings/cpp/rmi_cpp_client_example --transport=tcp --port=7070

# Run tests:
ctest --test-dir build -R "^cpp_binding\." --output-on-failure
```

To use the binding from a project outside this repository, add
`bindings/cpp/include` to the include path, link the precompiled
`bison_abi` shared library, and make sure `bison_c.h`/`rmi_c.h` (shipped
alongside `bison_abi` in the release zip — see `cmake/Packaging.cmake`) are
on the include path too:

```bash
g++ -std=c++20 -I bindings/cpp/include -I include myapp.cpp -o myapp -L build -lbison_abi
```

Quick-start snippet:

```cpp
#include "bison/bison.hpp"   // dynamic.hpp alone is enough without RMI
using namespace bdg::bison::abi;

int main() {
    dynamic p{"Player"_key};
    p["hp"_key] = 100;
    p["name"_key] = std::string{"hero"};
    std::cout << p["hp"_key].as<int32_t>();   // 100

    auto client = rmi::client::standalone();
    client.connect();
    auto calc = client.instantiate("Calculator"_key);
    dynamic args;
    args["a"_key] = 1.0f;
    args["b"_key] = 2.0f;
    dynamic result = calc.call("add"_key, args);   // calls the "add" remote method
    std::cout << result["result"_key].as<float>();
}
```

`bdg::bison::abi::key_t` (and, in test files, `bison_exception`) should
stay explicitly qualified at any declaration site reachable after `using
namespace bdg::bison::abi;` — glibc's `<sys/types.h>` (pulled in
transitively by many standard headers) also declares a global `key_t`
typedef, making a bare `key_t` ambiguous. This is the same, already-known
pitfall the internal C++ test suite documents (see
`tests/bison_c_tests.cpp`); `bindings/cpp/tests/dynamic_tests.cpp` and the
RMI examples show the qualified-alias workaround in practice.

---

## Python (`bindings/python/`)

Thin `ctypes` wrapper (`bindings/python/bison/`) exposing both APIs through
idiomatic Python syntax instead of raw C calls — e.g. `params.a = 10.0`
instead of `bison_set_float(params, bison_key("a"), 10.0f)`. Every handle is
wrapped in an RAII object (`Dynamic`, `Client`, `Server`, `Proxy`, `Future`);
none of the `bison_*_release` / `rmi_*_release` functions need to be called
directly — use `.release()`, a `with` block, or let the wrapper's
`__del__` clean up. No installation needed — import directly.

Fields support both attribute (`obj.field`) and dict (`obj["field"]`)
notation — pick whichever reads best; array-style numeric indices
(`obj[0]`) only work with the dict form, since `obj.0` isn't valid Python.

A field the C++ side declares as `bison::key_t` (e.g. an object's `"id"`, or
an enum-like selector) is a distinct field-variant type from `int32_t` — a
bare Python `int` handed to `obj["id"] = ...` is ambiguous between the two,
so it's always written as int32. Use `obj.set_key("id", value)` (*value* is
either an already-hashed int, e.g. from `key()`, or a name string hashed the
same way `"value"_key` would be in C++) to write one instead. Reading is
transparent: `obj["id"]` falls back to `bison_get_key()` automatically once
int/float/bool/string/object all fail by type mismatch. `obj.add_field_key(name,
value, meta=None)` is the `add_field()` counterpart for schema declarations.

**Requirements:** Python 3.x, `pytest` (optional, for tests)

```bash
# Run examples:
python bindings/python/examples/bison_example.py
python bindings/python/examples/rmi_standalone_example.py
python bindings/python/examples/rmi_server_example.py --transport=tcp --port=7070   # separate terminal
python bindings/python/examples/rmi_client_example.py --transport=tcp --port=7070

# Run tests:
python -m pytest bindings/python/tests -v
python -m unittest discover -s bindings/python/tests
```

Quick-start snippet:

```python
from bison import Dynamic

with Dynamic("Player") as p:
    p.hp = 100
    p.name = "hero"
    print(p.hp)   # 100

from bison.rmi import Client

with Client.standalone() as client:
    with client.instantiate("Calculator") as calc:
        result = calc.add(a=1.0, b=2.0)   # calls the "add" remote method
        print(result.result)
```

---

## C# (`bindings/csharp/`)

Source-generated `[LibraryImport]` P/Invoke wrapper (`bindings/csharp/Bison/`,
namespace `Bdg.Bison`) over the same `bison_abi` shared library, exposing
both APIs through three interchangeable access styles — pick whichever fits
the call site:

- **Indexer:** `obj["field"] = value`, `obj[0]` (array-like index) — always
  available, and the only option for numeric indices (`obj.0` isn't valid C#).
- **`dynamic`:** assign a `Dynamic` to a `dynamic`-typed variable and use
  ordinary member syntax: `dynamic d = obj; d.field = value; d.someMethod(a: 1, b: 2);`
  — backed by `DynamicObject`, C#'s equivalent of Python's
  `__getattr__`/`__setattr__`.
- **Typed:** `obj.Call("method", args)`, `obj.AddField(...)`, `obj.AddMethod(...)`, etc.

A field the C++ side declares as `bison::key_t` is a distinct field-variant
type from `int32_t` — the indexer's `Set()` always writes a plain `int` as
int32, so use `obj.SetKey("id", value)` (overloads take an already-hashed
`uint` or a name string to hash) to write one instead. Reading is
transparent: the indexer's `Get()` falls back to `GetKey()` automatically
once every other type check fails. `obj.AddFieldKey(name, value, meta)` is
the `AddField()` counterpart for schema declarations.

Every handle is an `IDisposable` RAII wrapper (`Dynamic`, `Client`, `Server`,
`Proxy`, `Future`) with a finalizer safety net, so `using`/`Dispose()` is
enough — none of the `bison_*_release` / `rmi_*_release` functions need to
be called directly.

Unlike C++'s `"name"_key` (a `constexpr` FNV-1a hash evaluated at compile
time), C# has no way to hash a field/method name before run time, so every
access funnels through `Bdg.Bison.Key.Of(name)`. That call is memoized in a
bounded (4096-entry) cache — the same tradeoff `bison.dynamic.key()` makes
with `functools.lru_cache(maxsize=4096)` on the Python side, for the same
reason: field/method names are drawn from a small, static, schema-defined
set reused across many calls, so caching turns most lookups into a
dictionary hit instead of a string encode + P/Invoke call, while staying
bounded so a caller hashing high-cardinality strings can't grow it forever.

**Requirements:** .NET 8 SDK

```bash
# Run examples:
dotnet run --project bindings/csharp/examples/BisonExample
dotnet run --project bindings/csharp/examples/RmiStandaloneExample
dotnet run --project bindings/csharp/examples/RmiServerExample -- --transport=tcp --port=7070   # separate terminal
dotnet run --project bindings/csharp/examples/RmiClientExample -- --transport=tcp --port=7070

# Run tests:
dotnet test bindings/csharp/Bison.Tests
```

`BISON_LIB` (see above) overrides the native library search here too; failing
that, `Native.cs` looks in `build/libbison_abi.{so,dylib}` /
`build/{Debug,Release}/bison_abi.dll` relative to the repo root before
falling back to the OS's normal library search.

Quick-start snippet:

```csharp
using Bdg.Bison;
using Bdg.Bison.Rmi;

using (var p = new Dynamic("Player"))
{
    dynamic d = p;
    d.hp = 100;
    d.name = "hero";
    Console.WriteLine(d.hp);   // 100
}

using var client = Client.Standalone();
client.Connect();
using var calc = client.Instantiate("Calculator");
dynamic proxy = calc;
dynamic result = proxy.add(a: 1.0f, b: 2.0f);   // calls the "add" remote method
Console.WriteLine(result.result);
```

`result` must stay `dynamic` (not the concrete `Dynamic` type) for `.result`
to resolve through `TryGetMember` at run time — a statically-typed
`Dynamic result` would need the indexer (`result["result"]`) instead, since
C# only defers member lookup to `DynamicObject` when the reference's static
type is `dynamic`.

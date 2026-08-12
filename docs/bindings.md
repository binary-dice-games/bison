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

RMI servers can gate incoming connections with `listen()`'s optional `auth`
parameter, which wraps the internal `auth_module_iface`
(`src/rmi/server/auth.hpp`) — the one `on_*` extension hook this binding
exposes (see this header's top-level doc comment). It receives the client's
`OP_CONNECT` payload and returns `true`/`false` to accept or reject the
connection, optionally writing an identity string through the
`std::string&` out-parameter:

```cpp
server.listen(dynamic(), [](const dynamic& payload, std::string& out_identity) {
  if (payload["token"_key].as<std::string>() != "secret") return false;
  out_identity = "user-42";
  return true;
});
```

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
`obj.set_key_at(index, value)` is the indexed counterpart, for the same
reason `obj[index] = value` can't dispatch to it from a bare `int`.

Every numeric-index (`obj[0]`) type `obj["field"]` supports also works at an
index, including `bool` (round-trips as a real `bool`, not silently coerced
to `int`), nested objects, and `None`. Vector-typed fields
(`obj["tags"] = [1, 2, 3]`, `bytes`/`bytearray` for `vector<uint8_t>`) are
named-field only — read them back the same way (`obj["tags"]` returns a
`list`/`bytes`); `obj.add_field("tags", [...])` is the schema-declaration
form. An empty list has no element-type information to infer from and
defaults to `vector<int32_t>`. `obj.serialize()` / `bison.deserialize(data)`
round-trip the compact binary wire format (`FORMAT.md`), the counterpart to
`to_json()`/`to_yaml()` for a self-contained (no key-name map needed) byte
representation.

`Server.listen()`'s optional `auth` parameter gates connections with a
Python callable, evaluated once per `OP_CONNECT` handshake. *auth* takes the
connect payload as a `Dynamic` and returns `(accepted, identity)`:

```python
def authenticate(payload):
    if payload["token"] != "secret":
        return False, ""
    return True, "user-42"

server.listen(auth=authenticate)
```

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
the `AddField()` counterpart for schema declarations. `obj.SetKeyAt(index,
value)` is the indexed counterpart, for the same reason `obj[index] = value`
can't dispatch to it from a bare `int`.

Every numeric-index (`obj[0]`) type `obj["field"]` supports also works at an
index, including `bool` (round-trips as a real `bool`, not silently coerced
to `int`), nested `Dynamic` objects, and `null`. Vector-typed fields
(`obj["tags"] = new[] { 1, 2, 3 }`; `bool[]`/`int[]`/`float[]`/`byte[]`) are
named-field only — read them back the same way (`obj["tags"]` returns the
matching array type, cast accordingly); `obj.AddField("tags", array)` is the
schema-declaration form. `obj.Serialize()` / `Dynamic.Deserialize(bytes)`
round-trip the compact binary wire format (`FORMAT.md`), the counterpart to
`ToJson()`/`ToYaml()` for a self-contained (no key-name map needed) byte
representation.

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

`Server.Listen()`'s optional `auth` parameter gates connections, evaluated
once per `OP_CONNECT` handshake. It takes the connect payload as a
`Dynamic` and returns `(bool Accepted, string Identity)`:

```csharp
server.Listen(auth: payload =>
    (string?)payload["token"] == "secret" ? (true, "user-42") : (false, string.Empty));
```

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

---

## Android (Java / Kotlin) (`bindings/android/`)

Unlike the other bindings, which load a precompiled `bison_abi` at run time
(`ctypes.CDLL`, `[LibraryImport]`, or link-time for C++), Android apps ship
their native libraries inside the APK and load them by name. This binding is
therefore two pieces instead of one: `bindings/android/jni/` is a small JNI
glue layer (`bison_jni`, built straight from this repo's own root
`CMakeLists.txt` — see [docs/building.md](building.md#building-for-android))
linked against `bison_abi`, and `bindings/android/bison-lib/` is the Java
package (`com.bdg.bison`) that calls it — `Dynamic p = new Dynamic("Player");
p.setInt("hp", 100);` instead of `bison_set_int(h, bison_key("hp"), 100)`.
Both `bison_abi.so` and `bison_jni.so` end up in the app's `jniLibs/<abi>/`;
`NativeLibrary.ensureLoaded()` (called from every public class's static
initializer) loads both, `bison_abi` first.

Field/method names hash through `Key.of(name)` (memoized, bounded at 4096
entries) the same way Python's `bison.dynamic.key()` and C#'s `Key.Of()` do
— Java has no way to hash a string literal at compile time either.
`Dynamic` is `AutoCloseable`, matching C#'s `IDisposable` choice: use a
try-with-resources block, or call `close()` directly.

```java
import com.bdg.bison.Dynamic;

try (Dynamic p = new Dynamic("Player")) {
    p.setInt("hp", 100);
    p.setString("name", "hero");
    System.out.println(p.getInt("hp"));   // 100
}
```

```java
import com.bdg.bison.rmi.Client;
import com.bdg.bison.rmi.Proxy;

try (Client client = Client.standalone()) {
    client.connect();
    try (Proxy calc = client.instantiate("Calculator", null)) {
        try (Dynamic args = new Dynamic()) {
            args.setInt("a", 1);
            args.setInt("b", 2);
            try (Dynamic result = calc.call("add", args)) {   // calls the "add" remote method
                System.out.println(result.getInt("value"));
            }
        }
    }
}
```

Method registration (`addMethod`) works from Java too, including as the
server side of an RMI class — the callback is invoked via a JNI upcall from
whatever native thread dispatches it (an RMI worker thread for a remote
call), which needs its own `JNIEnv` attached; the binding handles that
transparently:

```java
calc.addMethod("add", (self, params, result) ->
    result.setInt("value", params.getInt("a") + params.getInt("b")));
```

**Requirements:** Android NDK r26+, `compileSdk`/`targetSdk` 34, `minSdk` 24
(see [docs/building.md](building.md#building-for-android) for why 24, not
21, is the floor). No separate `bison_abi` build step to run by hand —
Gradle's `externalNativeBuild` drives it (see below).

```bash
cd bindings/android
./gradlew assembleDebug              # builds :bison-lib and :examples:BisonExample
./gradlew :bison-lib:connectedAndroidTest   # runs the binding's instrumented tests on a device/emulator
```

See [docs/examples.md](examples.md) for running the example app and its
instrumented tests on an emulator specifically.

### Gaps versus the internal C++ API

This is a first pass at the Android platform, so its API surface is smaller
than the Python/C#/C++ bindings':

- **Indexed (numeric) field access** (`obj[0]`) is not exposed — named
  fields only.
- **Class inheritance and cross-namespace lookup** aren't exposed.
  `Dynamic.registerClass(prototype)` covers registering a root (parentless)
  class in the global namespace — enough to host a class for RMI, which is
  all the example app needs — but there's no `instantiate(parent)` or
  `findClass()` yet.
- **Field/class/method attribute metadata** (`bison_attributes`) isn't
  exposed.
- **YAML text interop** isn't exposed (JSON is, via `toJson()`/`fromJson()`).
- **RMI**: only the `standalone` and TCP socket transports are bound (no
  named-pipe or `--transport=term` — neither is meaningful for an Android
  app process), only the synchronous `rmi_proxy_*`/`rmi_client_instantiate`
  calls (no `rmi_future_handle`/async), and `rmi_proxy_on_event` (server-
  pushed events) and `rmi_server_listen`'s `auth_handler` aren't exposed.

None of these are architectural dead ends — each is a straightforward
extension of the same JNI-glue-plus-Java-wrapper shape already in place for
the rest of the surface (see `bindings/android/jni/bison_jni.cpp` and
`rmi_jni.cpp`).

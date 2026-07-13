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

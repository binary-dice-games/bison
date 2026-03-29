# Bison

**Bison** is a C++20 library for serializing and deserializing objects in a compact binary format. It is conceptually similar to JSON — objects are self-describing, carrying both field names and values — but the wire format is binary rather than text, making it more efficient in terms of size and parsing speed.

Unlike Protocol Buffers (protobuf), Bison objects do not require a separate IDL file to describe the schema. The format is self-contained: every serialized object embeds its own field names and types. Objects can also hold callable methods, making them usable as live runtime entities rather than passive data bags.

## Features

- **Self-describing binary format** — field names and types are embedded in the serialized data; no external schema required.
- **Dynamic, heterogeneous objects** — a single `dynamic` object can hold fields of any supported type, including nested `dynamic` objects.
- **Method registration** — attach callable functions to objects at runtime; call them by name passing a `dynamic` as arguments and receiving a `dynamic` as the result.
- **Class hierarchy & inheritance** — register named classes with parent/child relationships; fields and methods are resolved through the inheritance chain.
- **Endian-safe serialization** — automatic byte-order conversion ensures portability between little- and big-endian platforms.
- **Attribute metadata** — attach custom, type-safe metadata to individual fields without changing the core variant type.
- **JSON interoperability** — convert a JSON string directly into a `dynamic` object via the bundled extension.
- **Compile-time string hashing** — field names are resolved to 32-bit FNV-1a hashes at compile time via the `"name"_key` literal, so name lookup has no string-comparison overhead at runtime.

## Requirements

| Requirement | Version |
|---|---|
| C++ standard | C++20 or later |
| CMake | 3.10 or later |
| [nlohmann/json](https://github.com/nlohmann/json) | bundled as git submodule |
| [libyaml](https://github.com/yaml/libyaml) | bundled as git submodule |
| [Google Test](https://github.com/google/googletest) | bundled as git submodule (tests only) |

## Building

```bash
# Clone with submodules
git clone --recurse-submodules https://github.com/carloslopezmdez/bison.git
cd bison

# Configure and build
cmake -B build
cmake --build build --config Debug

# Build with tests enabled
cmake -B build -DPACKAGE_TESTS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug
```

The project builds the C++ library target `bison` and the shared C API target `bison_c`. The Python binding loads `bison_c`, so make sure that target is built before running Python examples or tests.

To use the C++ library in your own CMake project:

```cmake
add_subdirectory(bison)
target_link_libraries(my_target PRIVATE bison)
```

Then include the single header:

```cpp
#include <bison.hpp>
```

## C++ Example

The repository includes a small C++ example target named `bison_examples` in `examples/main.cpp`.

### Build the example

From the repository root:

```bash
cmake -B build
cmake --build build --config Debug --target bison_examples
```

### Run the example

With the Visual Studio generator on Windows, the executable is emitted at `build/examples/Debug/bison_examples.exe`. You can run it directly:

```powershell
.\build\examples\Debug\bison_examples.exe
```

If you want to stay within CMake commands, use `cmake -E chdir` to launch it:

```bash
cmake -E chdir build/examples/Debug bison_examples.exe
```

The example output intentionally uses ASCII-only separators so it renders correctly in default Windows PowerShell and other terminals without additional encoding configuration.

## Performance Benchmark

The repository also includes a benchmark target named `bison_performance` in `examples/performance.cpp`. It compares the cost of implementing the same record-like object using a plain C++ class, `bison::dynamic`, and `nlohmann::json`.

The benchmark uses repeated samples with optional warm-up passes. Each result is reported as `min/median`, and the ratio columns are calculated from the median times. The `Serialize` and `Deserialize` rows reuse prebuilt objects and payloads so they isolate serialization cost instead of folding object construction into the same measurement.

The benchmark currently measures these operations over a configurable number of iterations:

- create / destroy
- field set / get
- method-style calls
- serialize
- deserialize

### Build the benchmark

Use `Release` for meaningful timing numbers:

```bash
cmake -B build
cmake --build build --config Release --target bison_performance
```

### Run the benchmark

From the repository root on Windows:

```powershell
.\build\examples\Release\bison_performance.exe 100000
```

The positional numeric argument is the iteration count per sample. If you omit it, the executable uses a built-in default.

You can also control the benchmark with flags:

```powershell
.\build\examples\Release\bison_performance.exe --iterations=100000 --samples=7 --warmup=2 --format=markdown
```

Supported output formats are `table`, `csv`, and `markdown`.

### Benchmark architecture

The benchmark harness is intentionally structured to keep timing comparisons fair and repeatable:

- **Three equivalent implementations**: each operation is implemented using a plain C++ record, `bison::dynamic`, and `nlohmann::json`.
- **Sample-based timing**: each row is measured across warm-up passes and multiple timed samples; reported values are `min/median` milliseconds.
- **Median-based ratios**: `dyn x` and `json x` are computed from median times, which reduces sensitivity to outliers.
- **State reset per sample**: mutating benchmarks (set/get and method-style call) rebuild their working state for each measured sample.
- **Prebuilt serialization fixtures**: serialize/deserialize rows reuse precomputed objects and payload buffers so those rows isolate serialization work.
- **Optimization guard**: benchmark paths feed values into a volatile sink to prevent dead-code elimination.

Together, this keeps the benchmark focused on representation overhead rather than setup noise.

## Python Binding

The `python/` package is a thin `ctypes` wrapper over the native `bison_c` shared library. It does not build the native code itself, so build the project first.

### Build the native library

From the repository root:

```bash
cmake -B build -DPACKAGE_TESTS=ON
cmake --build build --config Debug --target bison_c
```

On Windows, the binding looks for `build/Release/bison_c.dll` first and then `build/Debug/bison_c.dll`. If your DLL is somewhere else, point the binding at it explicitly:

```powershell
$env:BISON_LIB = (Resolve-Path .\build\Debug\bison_c.dll)
```

On Linux or macOS, set `BISON_LIB` to the full path of `libbison_c.so` or `libbison_c.dylib` if it is not found automatically.

### Run the Python examples

From the repository root:

```bash
python python/examples.py
```

The examples script imports `python.bison` directly from this repository, so no package installation step is required.

### Run the Python tests

The test file supports both `pytest` and the standard library `unittest` runner:

```bash
python -m pytest python/test_bison.py -v
```

```bash
python -m unittest python.test_bison
```

If `pytest` is not installed, install it with:

```bash
python -m pip install pytest
```

## Quick Start

All public symbols live in the `bdg::bison` namespace.

```cpp
#include <bison.hpp>
#include <sstream>

using namespace bdg::bison;

int main() {
    // --- Create an object with named fields ---
    dynamic obj{"MyClass"_key, {
        {"name"_key,  std::string{"Alice"}},
        {"score"_key, int32_t{42}},
        {"active"_key, true}
    }};

    // --- Access and modify fields ---
    obj["score"_key] = int32_t{100};
    std::string name = obj["name"_key].as<std::string>();

    // --- Serialize to a binary stream ---
    std::stringstream ss;
    obj.serialize(serializer(ss));

    // --- Deserialize back ---
    auto restored = dynamic::deserialize(deserializer(ss));
    int32_t score = (*restored)["score"_key].as<int32_t>();  // 100
}
```

## Core Concepts

### `dynamic` — the runtime object

`dynamic` is the central class. It holds a map of named fields (accessed by hashed key) and a map of named methods (callable functions). Fields can be any type in the supported variant:

| C++ type | Description |
|---|---|
| `std::monostate` | Empty / unset |
| `hash_t` (`uint32_t`) | Hashed key identifier |
| `key_t` | Hashable name |
| `bool` | Boolean |
| `int32_t` | 32-bit integer |
| `float` | 32-bit float |
| `std::shared_ptr<dynamic>` | Nested object |
| `std::string` | Text |
| `std::vector<bool>` | Boolean array |
| `std::vector<int32_t>` | Integer array |
| `std::vector<float>` | Float array |

### `dynamic_ptr` — owning pointer

`dynamic_ptr` is a thin `std::shared_ptr<dynamic>` subclass with convenience constructors:

```cpp
dynamic_ptr obj{"MyClass"_key, {{"x"_key, 1.0f}, {"y"_key, 2.0f}}};
obj->serialize(serializer(ss));
```

### Keys — compile-time string hashing

Field names are hashed to 32-bit integers at compile time using the `_key` user-defined literal:

```cpp
auto k = "position"_key;   // constexpr hash_t at compile time
obj[k] = 3.14f;

// Equivalent runtime construction
key_t k2{"position"};      // hashes the string in the constructor
```

### Serialization modes

**Standard** — includes field names (keys) in the output; fully self-describing:

```cpp
obj.serialize(serializer(out));
auto copy = dynamic::deserialize(deserializer(in));
```

**Template-based** — uses a pre-registered class definition as the schema; only field values are written, reducing output size:

```cpp
// Class must be registered before use
dynamic::addClass(0U, dynamic_ptr{"Point"_key, {{"x"_key, 0.0f}, {"y"_key, 0.0f}}});

point.serializeWithTemplate(serializer(out));
auto copy = dynamic::deserializeWithTemplate(deserializer(in));
```

### Method registration and invocation

Attach lambdas (or any `std::function`) to an object. A method receives the object itself (`self`) and a `dynamic` carrying the call arguments, and returns a `dynamic` result:

```cpp
dynamic obj{"Calculator"_key};

obj.addMethod("add"_key, [](dynamic& self, const dynamic& params) -> dynamic {
    int32_t a = params["a"_key].as<int32_t>();
    int32_t b = params["b"_key].as<int32_t>();

    dynamic result;
    result["value"_key] = a + b;
    return result;
});

dynamic args;
args["a"_key] = int32_t{3};
args["b"_key] = int32_t{4};

dynamic result = obj.call("add"_key, args);
int32_t sum = result["value"_key].as<int32_t>();  // 7
```

### Class inheritance

Register named classes with parent/child relationships. Fields and methods defined on a parent class are automatically available on child instances:

```cpp
// Define a base class with shared fields
auto base = dynamic_ptr{"Shape"_key, {{"color"_key, std::string{"red"}}}};
base->addMethod("describe"_key, [](dynamic& self, const dynamic&) -> dynamic {
    dynamic r;
    r["text"_key] = self["color"_key].as<std::string>() + " shape";
    return r;
});
dynamic::addClass(0U, base);

// Define a derived class
auto circle = dynamic_ptr{"Circle"_key, {{"radius"_key, 1.0f}}};
dynamic::addClass("Shape"_key, circle);

// Instantiate and use
dynamic c = dynamic::instantiate("Circle"_key);
c["color"_key];        // inherited from Shape
c.call("describe"_key, dynamic{});  // method inherited from Shape
```

### Field attributes

Attach arbitrary typed metadata to a field without altering the field's stored value:

```cpp
class Required : public attribute {};
class Range    : public attribute {
  public:
    Range(float lo, float hi) : lo(lo), hi(hi) {}
    float lo, hi;
};

field f{3.14f, attr<Required>(), attr<Range>(0.0f, 10.0f)};

if (f.findAttribute<Required>()) { /* field is required */ }
if (auto* r = f.findAttribute<Range>()) {
    // r->lo, r->hi
}
```

### JSON interoperability

Convert a JSON string into a `dynamic` object:

```cpp
#include <bison.hpp>

auto obj = bdg::bison::extensions::from_json(R"({"x": 1, "y": 2.5, "label": "hello"})");
int32_t x = (*obj)["x"].as<int32_t>();
```

The conversion maps JSON types as follows:

| JSON type | `field` type |
|---|---|
| `null` | `std::shared_ptr<dynamic>{}` (null ptr) |
| `boolean` | `bool` |
| `integer` | `int32_t` |
| `float` | `float` |
| `string` | `std::string` |
| `array` | `dynamic` with numeric indices |
| `object` | `dynamic` with hashed-string keys |

### User data

Attach arbitrary C++ objects to a `dynamic` without adding them to the serialized payload:

```cpp
class MyContext : public userdata {
  public:
    int session_id = 42;
};

dynamic obj;
obj.setUserdata(std::make_shared<MyContext>());

auto ctx = std::dynamic_pointer_cast<MyContext>(obj.getUserdata());
```

## API Reference

Full Doxygen-style documentation is embedded in `src/bison.hpp`. Generate HTML docs with:

```bash
doxygen Doxyfile
```

A `Doxyfile` can be generated with `doxygen -g` and configured to point `INPUT` at `src/`.

## Java Binding

The `java/` directory contains a JNA (Java Native Access) binding that mirrors the Python `ctypes` approach.  It wraps `libbison_c` and exposes a `Dynamic` class with the same feature set.

### Requirements

| Requirement | Version |
|---|---|
| Java | 11 or later |
| Maven | 3.6 or later |
| JNA | 5.14 (fetched automatically by Maven) |
| JUnit 5 | 5.10 (test scope, fetched automatically) |

### Build the native library

```bash
cmake -B build -DPACKAGE_TESTS=ON
cmake --build build --config Debug --target bison_c
```

### Run the Java examples

From the `java/` directory:

```bash
cd java
mvn compile exec:java -Dexec.mainClass=com.bdg.bison.examples.BisonExamples
```

If the shared library is not found automatically, set `BISON_LIB` first:

```bash
export BISON_LIB=$(pwd)/../build/libbison_c.so   # Linux
# macOS: export BISON_LIB=$(pwd)/../build/libbison_c.dylib
# Windows: set BISON_LIB=..\build\Debug\bison_c.dll
```

### Run the Java tests

```bash
cd java
mvn test
```

### Quick-start Java snippet

```java
try (Dynamic obj = new Dynamic()) {
    obj.setInt("hp",      100);
    obj.setFloat("speed", 9.5f);
    obj.setBool("alive",  true);
    obj.setString("name", "hero");

    System.out.println(obj.getInt("hp"));     // 100
    System.out.println(obj.getFloat("speed")); // 9.5
}

// JSON import
try (Dynamic root = Dynamic.fromJson("{\"x\": 1, \"y\": 2}")) {
    System.out.println(root.getInt("x")); // 1
}
```

## C# Binding

The `csharp/` directory contains a P/Invoke binding for .NET 6+.  It wraps `libbison_c` and exposes a `Dynamic` class that implements `IDisposable`.

### Requirements

| Requirement | Version |
|---|---|
| .NET SDK | 6.0 or later |
| xUnit | 2.7 (test project, fetched automatically by NuGet) |

### Build the native library

```bash
cmake -B build -DPACKAGE_TESTS=ON
cmake --build build --config Debug --target bison_c
```

### Run the C# examples

From the `csharp/` directory:

```bash
cd csharp
dotnet run -- examples
```

Set `BISON_LIB` if the shared library is not found automatically (same approach as the Java binding).

### Run the C# tests

```bash
cd csharp/tests
dotnet test
```

### Quick-start C# snippet

```csharp
using Bdg.Bison;

using var obj = new Dynamic();
obj.SetInt("hp",     100);
obj.SetFloat("speed", 9.5f);
obj.SetBool("alive", true);
obj.SetString("name", "hero");

Console.WriteLine(obj.GetInt("hp"));      // 100
Console.WriteLine(obj.GetFloat("speed")); // 9.5

// JSON import
using var root = Dynamic.FromJson("{\"x\": 1, \"y\": 2}");
Console.WriteLine(root.GetInt("x")); // 1
```

## Performance Optimizations

The library has been optimised for serialization and deserialization throughput.  The changes described below are in `src/bison.hpp`.

### 1. Compiler-intrinsic `byte_swap`

The byte-swapping function that converts scalars to network (big-endian) byte order now uses `__builtin_bswap16/32/64` on GCC and Clang, and `_byteswap_ushort/ulong/uint64` on MSVC.  The compiler lowers these intrinsics to a single `bswap` (x86) or `rev` (ARM) instruction, which is faster than the loop-based fallback retained for other compilers.

### 2. Buffer-based serializer / deserializer

Two new classes — `buffer_serializer` and `buffer_deserializer` — write and read directly from a `std::vector<char>` (or `const char*` / length pair) without going through an `std::ostream` / `std::istream`.  Every `write()` / `read()` call on the stream classes dispatches virtually; the buffer variants eliminate that overhead entirely.

```cpp
// --- Serialize ---
buffer_serializer out;
obj.serialize(out);
std::vector<char> bytes = out.release();

// --- Deserialize ---
buffer_deserializer in(bytes);
auto copy = dynamic::deserialize(in);
```

`dynamic`, `field`, `serializer`-style overloads, and `serializeWithTemplate` / `deserializeWithTemplate` all have buffer variants.  The performance benchmark in `examples/performance.cpp` includes `Serialize (buf)` and `Deserialize (buf)` rows that compare the two approaches side-by-side.

### 3. `fields_` retained as `std::map` (ordering is required)

`fields_` is used both as a named-field dictionary (keys with the high bit set) and as an array (small numeric keys 0, 1, 2, …).  `std::map` guarantees that entries are visited in ascending key order, which is essential in two ways:

- **Array semantics** — numeric indices must be iterated in order 0, 1, 2, … for `size()` and field iteration to be correct.
- **Template serialization** — `serializeWithTemplate` writes field *values* in the order they appear in the class prototype's map; `deserializeWithTemplate` must read them back in exactly the same order.  With `std::unordered_map` the iteration order is non-deterministic across process restarts, so template-mode round-trips would silently swap field values.

`fields_` therefore stays as `std::map<key_t, field>`.  The `size()` and `clear()` methods use `lower_bound(0x80000000u)` to efficiently separate the numeric portion of the map from the named portion in O(log n) time.

### Running the benchmark

Build in `Release` for meaningful timings:

```bash
cmake -B build
cmake --build build --config Release --target bison_performance
./build/examples/bison_performance --iterations=100000 --format=table
```

The output now includes `Serialize (buf)` and `Deserialize (buf)` rows alongside the original stream-based rows, making it easy to see the speedup from the buffer path.

## License

MIT License © 2025 Binary Dice Games.  
See [LICENSE](LICENSE) for the full text.

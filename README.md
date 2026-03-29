# Bison

**Bison** is a C++17 library for serializing and deserializing objects in a compact binary format. It is conceptually similar to JSON — objects are self-describing, carrying both field names and values — but the wire format is binary rather than text, making it more efficient in terms of size and parsing speed.

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
| C++ standard | C++17 or later |
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
cmake --build build

# Build with tests enabled
cmake -B build -DPACKAGE_TESTS=ON
cmake --build build
ctest --test-dir build -C Debug
```

The library compiles as a static library (`libbison.a`). To use it in your own CMake project:

```cmake
add_subdirectory(bison)
target_link_libraries(my_target PRIVATE bison)
```

Then include the single header:

```cpp
#include <bison.hpp>
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

## License

MIT License © 2025 Binary Dice Games.  
See [LICENSE](LICENSE) for the full text.

# Bison

**Bison** is a C++20 library for serializing and deserializing objects in a compact binary format. It is conceptually similar to JSON — objects are self-describing, carrying both field names and values — but the wire format is binary rather than text, making it more efficient in size and parsing speed. Unlike Protocol Buffers, no external IDL schema is required.

## Features

- **Self-describing binary format** — field names and types are embedded in serialized data; no external schema required.
- **Dynamic, heterogeneous objects** — a single `dynamic` object holds fields of any supported type, including nested objects.
- **Method registration** — attach callable functions to objects at runtime; call them by name.
- **Class hierarchy & inheritance** — register named classes with parent/child relationships; fields and methods resolve through the inheritance chain.
- **Endian-safe serialization** — automatic byte-order conversion for portability.
- **Attribute metadata** — attach custom typed metadata to fields without changing the variant type.
- **JSON interoperability** — convert JSON strings directly into `dynamic` objects.
- **Compile-time string hashing** — field names resolve to 32-bit FNV-1a hashes at compile time via `"name"_key`.
- **Buffer serializer** — `buffer_serializer` / `buffer_deserializer` for zero-virtual-dispatch I/O.

## Requirements

| Requirement | Version |
|---|---|
| C++ standard | C++20 or later |
| CMake | 3.11 or later |
| nlohmann/json | bundled as git submodule |
| libyaml | bundled as git submodule |
| Google Test | bundled as git submodule (tests only) |

## Building

```bash
git clone --recurse-submodules https://github.com/carloslopezmdez/bison.git
cd bison
cmake -B build
cmake --build build --config Debug

# With tests:
cmake -B build -DPACKAGE_TESTS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug
```

For Linux / WSL setup and CMake integration details, see [docs/building.md](docs/building.md).

## Quick Start

All public symbols live in the `bdg::bison` namespace.

```cpp
#include <bison.hpp>
#include <sstream>
using namespace bdg::bison;

int main() {
    dynamic obj{"MyClass"_key, {
        {"name"_key,  std::string{"Alice"}},
        {"score"_key, int32_t{42}},
        {"active"_key, true}
    }};

    obj["score"_key] = int32_t{100};
    std::string name = obj["name"_key].as<std::string>();

    std::stringstream ss;
    obj.serialize(stream_serializer(ss));

    auto restored = dynamic::deserialize(stream_deserializer(ss));
    int32_t score = (*restored)["score"_key].as<int32_t>();  // 100
}
```

## Core Concepts

### `dynamic` — runtime object

Holds a map of named fields (accessed by hashed key) and a map of callable methods. Field types:

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
| `std::vector<bool/int32_t/float/uint8_t>` | Typed arrays |

### Keys — compile-time hashing

```cpp
auto k = "position"_key;  // constexpr hash_t
obj[k] = 3.14f;
```

### Serialization modes

```cpp
// Standard — self-describing, includes field names:
obj.serialize(stream_serializer(out));
auto copy = dynamic::deserialize(stream_deserializer(in));

// Buffer variant (faster, no virtual dispatch):
buffer_serializer buf;
obj.serialize(buf);
std::vector<char> bytes = buf.release();

// Schema-driven — compact, omits field keys (requires matching class registry):
obj.serializeWithSchema(stream_serializer(out));
auto copy = dynamic::deserializeWithSchema(stream_deserializer(in));
```

### Method registration

```cpp
dynamic obj{"Calculator"_key};
obj.addMethod("add"_key, [](dynamic& self, const dynamic& params) -> dynamic {
    dynamic result;
    result["value"_key] = params["a"_key].as<int32_t>() + params["b"_key].as<int32_t>();
    return result;
});
dynamic args;
args["a"_key] = int32_t{3};
args["b"_key] = int32_t{4};
dynamic result = obj.call("add"_key, args);
```

### Class inheritance

```cpp
auto base = dynamic_ptr{"Shape"_key, {{"color"_key, std::string{"red"}}}};
dynamic::addClass(0U, base);

auto circle = dynamic_ptr{"Circle"_key, {{"radius"_key, 1.0f}}};
dynamic::addClass("Shape"_key, circle);

dynamic c = dynamic::instantiate("Circle"_key);
c["color"_key];               // inherited from Shape
c.call("describe"_key, {});   // method inherited from Shape
```

### Field attributes

```cpp
class Range : public attribute {
  public: Range(float lo, float hi) : lo(lo), hi(hi) {} float lo, hi;
};

field f{3.14f, attr<Required>(), attr<Range>(0.0f, 10.0f)};
if (auto* r = f.findAttribute<Range>()) { /* r->lo, r->hi */ }
```

### JSON interoperability

```cpp
auto obj = bdg::bison::extensions::from_json(R"({"x": 1, "y": 2.5})");
int32_t x = (*obj)["x"].as<int32_t>();
```

### User data

```cpp
class MyContext : public userdata { public: int session_id = 42; };
obj.setUserdata(std::make_shared<MyContext>());
auto ctx = std::dynamic_pointer_cast<MyContext>(obj.getUserdata());
```

## RMI — Remote Method Invocation

The RMI subsystem lets a client invoke methods and read/write fields on objects hosted by a server, over a transport of choice. The protocol is request/response with async futures; the server can also push events to connected clients.

**Transports:** TCP socket (`socket_*_transport`), in-memory queues (`memory_*_transport`), and stdin/stdout (`stdio_*_transport`). The `standalone` class combines client and server in-process with no serialization overhead.

**Server** — register classes, then listen:

```cpp
#include <rmi/server.hpp>
using namespace bdg::bison;

auto proto = dynamic_ptr{"Calculator"_key, {{"result"_key, int32_t{0}}}};
proto->addMethod("add"_key, [](dynamic& self, const dynamic& p) -> dynamic {
    self["result"_key] = p["a"_key].as<int32_t>() + p["b"_key].as<int32_t>();
    return dynamic{};
});
dynamic::addClass(0U, proto);

rmi::server srv{rmi::socket_server_transport{}};
srv.listen(dynamic{});   // blocks until stopped
```

**Client** — connect, instantiate a remote object, call methods:

```cpp
#include <rmi/client.hpp>
using namespace bdg::bison;

rmi::client c{rmi::socket_client_transport{"localhost", 7777}};
c.connect();

auto proxy = c.instantiate(0U, "Calculator"_key, dynamic{}).get();

dynamic args;
args["a"_key] = int32_t{10};
args["b"_key] = int32_t{3};
proxy.call("add"_key, args).get();

auto snapshot = proxy.get().get();   // retrieve all fields
int32_t result = snapshot["result"_key].as<int32_t>();  // 13

c.destroy(std::move(proxy));
c.disconnect();
```

Key proxy operations: `set(fields)`, `get()`, `get(projection)`, `clear()`, `call(name, args)`, `onEvent(name, handler)`.

See [docs/examples.md](docs/examples.md) for building and running the socket and stdio PTY example programs.

## API Reference

Full Doxygen-style documentation is embedded in `src/bison/bison.hpp`. Generate HTML docs with:

```bash
doxygen Doxyfile
```

## Further Documentation

| Document | Contents |
|---|---|
| [docs/building.md](docs/building.md) | WSL/Linux setup, CMake integration |
| [docs/examples.md](docs/examples.md) | Running C++, RMI socket, stdio PTY, and benchmark examples |
| [docs/bindings.md](docs/bindings.md) | Python, Java, C# binding setup and usage |
| [docs/performance.md](docs/performance.md) | Benchmark architecture and optimization notes |
| [FORMAT.md](FORMAT.md) | Binary wire format specification |

## License

MIT License © 2025 Binary Dice Games. See [LICENSE](LICENSE) for the full text.

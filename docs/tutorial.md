# Tutorial: Getting Started with Bison

This is a beginner-friendly, example-driven walkthrough of the Bison C++
library. It assumes you've already built the project — see
[building.md](building.md) if not. Every snippet below is drawn from (or
directly adapted from) `examples/bison_example.cpp` and
`examples/rmi_standalone_example.cpp`, which you can build and run to see
this same material executed:

```bash
cmake --build build --config Debug --target bison_examples
./build/examples/bison_examples
```

All public symbols live in namespace `bdg::bison`; the snippets below assume
`using namespace bdg::bison;` and `#include "src/bison/bison.hpp"`.

## Table of contents

1. [Your first object](#1-your-first-object)
2. [Keys — how field names work](#2-keys--how-field-names-work)
3. [Field types](#3-field-types)
4. [Serialization](#4-serialization)
5. [Methods](#5-methods)
6. [Class hierarchy and inheritance](#6-class-hierarchy-and-inheritance)
7. [Namespaces](#7-namespaces)
8. [JSON and YAML import](#8-json-and-yaml-import)
9. [Field attributes](#9-field-attributes)
10. [User data](#10-user-data)
11. [Arrays and iteration](#11-arrays-and-iteration)
12. [A first taste of RMI](#12-a-first-taste-of-rmi)
13. [Where to go next](#13-where-to-go-next)

## 1. Your first object

`dynamic` is Bison's central type: a heterogeneous, self-describing object
that holds named fields and callable methods, similar in spirit to a JSON
object but with a compact binary wire format.

```cpp
dynamic obj{"Person"_key};
obj["name"_key] = std::string{"Alice"};
obj["age"_key] = int32_t{30};
obj["active"_key] = true;

std::cout << obj["name"_key].as<std::string>() << "\n";   // Alice
std::cout << obj.as<std::string>("name"_key) << "\n";      // same, shorthand
```

`obj["age"_key]` returns a `field&`; `.as<T>()` reads it as type `T`. A
`field` is type-checked once set — assigning a different type to an
already-set field throws `std::runtime_error`.

## 2. Keys — how field names work

Field and class names aren't compared as strings at runtime. `"name"_key` is
a user-defined literal that computes a 32-bit FNV-1a hash *at compile time*,
producing a `key_t` (implicitly convertible to the underlying `hash_t`):

```cpp
constexpr hash_t k1 = "velocity"_key;
hash_t k2 = hash("velocity");           // same value, computed at runtime
std::cout << std::boolalpha << (k1 == k2) << "\n";   // true

std::string field_name = "position";
key_t k3{field_name};                    // runtime key_t from a std::string
std::cout << (k3.id == "position"_key.id) << "\n";   // true
```

Named keys always have their most-significant bit set, so they never
collide with the small numeric indices (`0, 1, 2, …`) used for array-style
access (§11). See [FORMAT.md](../FORMAT.md) §1.3 for the exact hash formula
if you need to reproduce it in another language.

## 3. Field types

A `field` is a variant that can hold any one of:

| C++ type | Notes |
|---|---|
| `std::monostate` | empty / unset |
| `hash_t`, `key_t` | hashed identifiers, usable as ordinary field values too |
| `bool` | |
| `int32_t` | |
| `float` | |
| `std::string` | |
| `std::shared_ptr<dynamic>` (`dynamic_ptr`) | a nested object; a *null* `dynamic_ptr` is distinct from an empty/monostate field |
| `std::vector<bool>`, `std::vector<int32_t>`, `std::vector<float>` | typed arrays |
| `std::vector<uint8_t>` | raw byte blob |

There is no 64-bit integer or double type, and unsigned/enum values must be
represented with one of the above (typically `int32_t`).

```cpp
field f_bytes{std::vector<uint8_t>{0x00, 0xFF, 0x42}};
const auto& blob = f_bytes.as<std::vector<uint8_t>>();

field f_null_obj{std::shared_ptr<dynamic>{}};
std::cout << f_null_obj.is<dynamic_ptr>() << "\n";       // true: it's a (null) pointer field
std::cout << f_null_obj.is<std::monostate>() << "\n";    // false: not the same as "unset"
```

Nested objects work exactly like top-level ones:

```cpp
dynamic_ptr address{0U, {{"street"_key, std::string{"123 Main St"}},
                          {"city"_key, std::string{"Springfield"}}}};
obj["address"_key] = dynamic_ptr{address};
std::cout << (*obj["address"_key].as<dynamic_ptr>())["city"_key].as<std::string>() << "\n";
```

## 4. Serialization

Bison offers two orthogonal choices: **which wire format** (self-describing
vs. schema-driven) and **which I/O backend** (`std::iostream` vs. an
in-memory buffer). See [FORMAT.md](../FORMAT.md) for the exact byte layout.

### Standard (self-describing) format

Field names travel on the wire as hashes, so the receiver needs no prior
knowledge of the object's shape:

```cpp
std::stringstream ss;
{
  stream_serializer out{ss};
  obj.serialize(out);
}

stream_deserializer in{ss};
dynamic restored = dynamic::deserialize(in);
std::cout << restored["name"_key].as<std::string>() << "\n";
```

### Buffer variant

`buffer_serializer`/`buffer_deserializer` do the same job over
`bdg::bison::buffer` (`std::vector<uint8_t>`) instead of a stream, avoiding
`std::iostream`'s virtual-dispatch overhead — useful when you're about to
hand the bytes to a socket or file API directly:

```cpp
buffer_serializer bser;
obj.serialize(bser);
bdg::bison::buffer bytes = bser.release();

buffer_deserializer bdes{bytes};
dynamic restored = dynamic::deserialize(bdes);
```

### Schema-driven (compact) format

When both sides have registered the same class prototype (§6), field *names*
can be omitted entirely from the wire — only values are written, in
prototype order:

```cpp
dynamic::addClass(0U, dynamic_ptr{"Vector3"_key,
    {{"x"_key, float{0}}, {"y"_key, float{0}}, {"z"_key, float{0}}}});

dynamic v = dynamic::instantiate("Vector3"_key);
v["x"_key] = 1.0f; v["y"_key] = 2.0f; v["z"_key] = 3.0f;

buffer_serializer bser;
v.serializeWithSchema(bser);            // smaller than serialize() would produce
bdg::bison::buffer bytes = bser.release();

buffer_deserializer bdes{bytes};
dynamic restored = dynamic::deserializeWithSchema(bdes);
```

The reader and writer **must** have identical class registries (same
namespace, class name, field set, and registration order) — a mismatch
silently corrupts the decoded object rather than raising an error, so this
mode is best reserved for messages exchanged between processes built from
the same schema definitions (e.g. the RMI envelope itself, §12).

> **Note:** `addClass` registers into a single process-wide registry, and
> registering a duplicate `(namespace, class name)` pair fails (returns
> `false`) rather than overwriting the existing prototype. If you copy
> several of this tutorial's snippets into one program, either give each
> snippet's classes distinct names or clear the registry between them with
> `dynamic::getRegistry().wlock()->clear();` — see how
> `examples/bison_example.cpp` does this between its own sections.

## 5. Methods

`addMethod` attaches a `std::function<dynamic(dynamic& self, const dynamic& params)>`
to an object, callable later by name via `call()`:

```cpp
dynamic calc{"Calculator"_key};
calc["total"_key] = int32_t{0};

calc.addMethod("accumulate"_key, [](dynamic& self, const dynamic& params) -> dynamic {
    int32_t total = self["total"_key].as<int32_t>() + params["n"_key].as<int32_t>();
    self["total"_key] = total;
    dynamic result;
    result["total"_key] = total;
    return result;
});

for (int i = 1; i <= 5; ++i) {
  dynamic p;
  p["n"_key] = int32_t{i};
  calc.call("accumulate"_key, p);
}
std::cout << calc["total"_key].as<int32_t>() << "\n";   // 15
```

`self` is mutable, so a method can update the object's own state. Calling an
unregistered method name throws `std::runtime_error`.

## 6. Class hierarchy and inheritance

`addClass` registers a prototype `dynamic` in a global registry; instances
created with `instantiate` resolve fields and methods through the parent
chain on first access (and cache the result on the instance afterward):

```cpp
auto shape = dynamic_ptr{"Shape"_key, {{"color"_key, std::string{"black"}}}};
shape->addMethod("describe"_key, [](dynamic& self, const dynamic&) -> dynamic {
    dynamic result;
    result["text"_key] = self["color"_key].as<std::string>() + " shape";
    return result;
});
dynamic::addClass(0U, shape);                    // 0U = global namespace, no parent

auto circle = dynamic_ptr{"Circle"_key, {{"radius"_key, 1.0f}}};
dynamic::addClass(0U, circle, "Shape"_key);       // parent = "Shape"_key

dynamic c = dynamic::instantiate("Circle"_key);
c["radius"_key] = 5.0f;
c["color"_key] = std::string{"red"};              // overrides the inherited default

dynamic desc = c.call("describe"_key, dynamic{}); // "describe" is inherited from Shape
std::cout << desc["text"_key].as<std::string>() << "\n";  // "red shape"

std::cout << std::boolalpha << (c.findClass("Shape"_key) != nullptr) << "\n";  // true
```

Registering a class whose parent chain would cycle back to itself fails
(`addClass` returns `false`); registering a duplicate name in the same
namespace also fails.

## 7. Namespaces

Every class lives in a namespace — `0U` for the global/default namespace,
or any `"name"_key` you choose. Two classes with the same name in different
namespaces don't collide:

```cpp
auto math_table = dynamic_ptr{"table"_key, {{"rows"_key, int32_t{0}}, {"cols"_key, int32_t{0}}}};
dynamic::addClass("math"_key, math_table);

auto ikea_table = dynamic_ptr{"table"_key, {{"legs"_key, int32_t{4}}}};
dynamic::addClass("ikea"_key, ikea_table);

dynamic mt = dynamic::instantiate("math"_key, "table"_key);
dynamic it = dynamic::instantiate("ikea"_key, "table"_key);
```

`instantiate(ns, klass)` writes `ns` onto the new instance's `__namespace`
field so subsequent field/method/class lookups go straight to the right
namespace's registry. Inheritance (`addClass(ns, klass, parent)`) resolves
`parent` within the *same* namespace only.

## 8. JSON and YAML import

`extensions::from_json` and `extensions::from_yaml` parse text into a
`dynamic_ptr`, mapping JSON/YAML objects to named fields, arrays/sequences to
numeric-indexed `dynamic` objects, and scalars to the closest matching field
type:

```cpp
auto parsed = extensions::from_json(R"({
  "name": "Alice", "age": 30, "tags": ["c++", "bison"],
  "address": {"city": "Springfield"}
})");
std::cout << (*parsed)["age"].as<int32_t>() << "\n";                       // 30
std::cout << (*(*parsed)["address"].as<dynamic_ptr>())["city"].as<std::string>() << "\n";
auto tags = (*parsed)["tags"].as<dynamic_ptr>();
std::cout << (*tags)[0].as<std::string>() << " (" << tags->size() << " tags)\n";

auto cfg = extensions::from_yaml("server:\n  host: localhost\n  port: 8080\ndebug: true\n");
std::cout << (*(*cfg)["server"].as<dynamic_ptr>())["port"].as<int32_t>() << "\n";  // 8080
```

Fields imported this way can be read and modified exactly like any other
`dynamic` object's fields.

## 9. Field attributes

Attributes attach arbitrary typed metadata to a `field` without touching its
stored value or its wire representation — they are never serialized:

```cpp
class Range : public attribute {
 public:
  Range(float lo, float hi) : lo(lo), hi(hi) {}
  float lo, hi;
};

field f{3.14f, attr<Range>(0.0f, 10.0f)};
if (auto* r = f.findAttribute<Range>()) {
  std::cout << "[" << r->lo << ", " << r->hi << "]\n";
}
```

## 10. User data

`userdata` attaches an arbitrary application-defined object to a `dynamic`
instance at runtime. Like attributes, it's purely in-memory: serializing and
deserializing an object never carries userdata along.

```cpp
class RenderState : public userdata {
 public:
  explicit RenderState(int id) : gpu_handle(id) {}
  int gpu_handle;
};

dynamic mesh{"Mesh"_key};
mesh.setUserdata(std::make_shared<RenderState>(42));

if (auto rs = std::dynamic_pointer_cast<RenderState>(mesh.getUserdata())) {
  std::cout << rs->gpu_handle << "\n";   // 42
}
```

## 11. Arrays and iteration

Numeric keys (`0, 1, 2, …`) turn a `dynamic` into an ordered sequence that
can coexist with named fields on the same object:

```cpp
dynamic list;
list[0] = std::string{"red"};
list[1] = std::string{"green"};
list[2] = std::string{"blue"};
list["label"_key] = std::string{"colors"};

std::cout << list.size() << "\n";        // 3 (numeric entries only)
list.erase(1);                            // removes index 1
list.clear();                             // removes ALL numeric entries...
std::cout << list["label"_key].as<std::string>() << "\n";  // ...but named fields survive
```

`forEach` visits every `(key, field)` pair in ascending key order (numeric
indices first, then named keys, since named keys always have their
most-significant bit set):

```cpp
obj.forEach([](key_t k, const field& f) {
  if (k.id < 0x80000000u) {
    std::cout << "index " << k.id << "\n";
  } else {
    std::cout << "named field\n";
  }
});
```

## 12. A first taste of RMI

RMI (Remote Method Invocation) lets a client call methods and read/write
fields on objects that live on a server, over a transport of your choice
(TCP, named pipe, in-memory, stdio, or an interactive terminal hop — see the
"RMI" section of the [README](../README.md)). The easiest way to try it
without spinning up two processes is `rmi::standalone`, which runs
client and server logic in-process with no transport or serialization at
all — the same proxy API you'd use with a real `rmi::client`:

```cpp
#include "src/rmi/rmi.hpp"
using namespace bdg::bison;

auto proto = dynamic_ptr{"Calculator"_key, {{"result"_key, int32_t{0}}}};
proto->addMethod("add"_key, [](dynamic& self, const dynamic& p) -> dynamic {
    self["result"_key] = p["a"_key].as<int32_t>() + p["b"_key].as<int32_t>();
    return dynamic{};
});
dynamic::addClass(0U, proto);

rmi::standalone sa;
auto proxy = sa.instantiate(0U, "Calculator"_key).get();

dynamic args;
args["a"_key] = int32_t{2};
args["b"_key] = int32_t{5};
proxy.call("add"_key, std::move(args)).get();

auto snapshot = proxy.get().get();                          // retrieve all fields
std::cout << snapshot["result"_key].as<int32_t>() << "\n";  // 7

sa.destroy(std::move(proxy));
```

Swapping `rmi::standalone` for a real `rmi::server`/`rmi::client` pair over
a transport (e.g. `rmi::socket_server_transport`/`rmi::socket_client_transport`)
uses the identical proxy API — see the README's RMI section for that
version, and [examples.md](examples.md) for running it as two separate
processes, including the `--transport=term` hop mode.

## 13. Where to go next

- [README.md](../README.md) — feature overview and quick API reference.
- [docs/examples.md](examples.md) — building and running every example binary, RMI transports, and the performance benchmark.
- [docs/building.md](building.md) — platform-specific build instructions and CMake integration.
- [docs/bindings.md](bindings.md) — using Bison from Python.
- [docs/performance.md](performance.md) — benchmark methodology and optimization notes.
- [FORMAT.md](../FORMAT.md) — the binary wire format, for interop with non-C++ implementations.

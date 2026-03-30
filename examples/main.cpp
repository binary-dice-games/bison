// MIT License © 2025 Binary Dice Games
// examples/main.cpp
//
// Detailed, runnable examples demonstrating every major feature of the Bison
// library.  Each example is a self-contained function.  Run the executable to
// see the output.

#include <iostream>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "src/core/bison.hpp"

using namespace bdg::bison;

// ─────────────────────────────────────────────────────────────────────────────
// Helper: print a separator line for readability
// ─────────────────────────────────────────────────────────────────────────────
static void section(const char* title) {
  static int index = 0;
  std::cout << "\n==========================================\n";
  std::cout << "  " << ++index << ". " << title << "\n";
  std::cout << "==========================================\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 1: Hashing and keys
//
// Field names in Bison are stored and compared as 32-bit FNV-1a hashes.
// The "_key" user-defined literal computes the hash at compile time so there
// is zero runtime string-comparison overhead.
// ─────────────────────────────────────────────────────────────────────────────
static void example_hashing() {
  section("Hashing and keys");

  // Compile-time hash via the _key literal
  constexpr hash_t k1 = "velocity"_key;

  // Runtime hash – produces the same value
  hash_t k2 = hash("velocity");

  std::cout << "\"velocity\"_key == hash(\"velocity\"): "
            << std::boolalpha << (k1 == k2) << "\n";

  // key_t wraps a hash and is implicitly convertible to hash_t
  bdg::bison::key_t k3 = "score"_key;
  std::cout << "score hash (hex): 0x" << std::hex << k3.id << std::dec << "\n";

  // Named keys always have the high bit set so they never collide with small
  // numeric array indices (0, 1, 2, …).
  std::cout << "High bit set on named key: "
            << std::boolalpha << bool(k3.id & 0x80000000u) << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 2: field – the variant value type
//
// A field can hold any of the supported types.  It enforces type safety: once
// set to a type, only that same type can be assigned again (or it throws).
// ─────────────────────────────────────────────────────────────────────────────
static void example_field() {
  section("field - the variant value type");

  // ── Construction ──────────────────────────────────────────────────────────
  field f_empty;                          // monostate (empty)
  field f_bool  {true};
  field f_int   {int32_t{42}};
  field f_float {3.14f};
  field f_str   {std::string{"hello"}};   // or just f_str{"hello"}
  field f_vi    {std::vector<int32_t>{1, 2, 3}};

  // ── Type checking ─────────────────────────────────────────────────────────
  std::cout << "empty is monostate : " << f_empty.is<std::monostate>() << "\n";
  std::cout << "f_int is int32_t   : " << f_int.is<int32_t>()          << "\n";
  std::cout << "f_int is float     : " << f_int.is<float>()            << "\n";

  // ── Access ────────────────────────────────────────────────────────────────
  int32_t  n = f_int.as<int32_t>();   // explicit typed access
  int32_t  m = f_int;                 // implicit conversion
  std::cout << "int value via as<> : " << n << "\n";
  std::cout << "int value implicit : " << m << "\n";

  // as<T>() on an empty field initialises it to the default value.
  f_empty.as<float>(9.9f);
  std::cout << "lazy-init empty field to float 9.9: " << f_empty.as<float>() << "\n";

  // ── Type-safe assignment ──────────────────────────────────────────────────
  f_int = int32_t{99};         // OK – same type
  try {
    f_int = float{1.0f};       // throws – wrong type
  } catch (const std::runtime_error& e) {
    std::cout << "Expected error: " << e.what() << "\n";
  }

  // ── Attributes ────────────────────────────────────────────────────────────
  // Attach arbitrary typed metadata without touching the stored value.
  class Required : public attribute {};
  class Range : public attribute {
   public:
    Range(float lo, float hi) : lo(lo), hi(hi) {}
    float lo, hi;
  };

  field f_attr{int32_t{5}, attr<Required>(), attr<Range>(0.f, 10.f)};

  if (f_attr.findAttribute<Required>()) {
    std::cout << "Field is required\n";
  }
  if (auto* r = f_attr.findAttribute<Range>()) {
    std::cout << "Range: [" << r->lo << ", " << r->hi << "]\n";
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 3: dynamic – the runtime object
//
// A dynamic object is a heterogeneous property bag: named fields, callable
// methods, an optional class tag, and optional userdata.
// ─────────────────────────────────────────────────────────────────────────────
static void example_dynamic_basics() {
  section("dynamic - the runtime object");

  // ── Named fields ──────────────────────────────────────────────────────────
  dynamic obj{"Person"_key};
  obj["name"_key]  = std::string{"Alice"};
  obj["age"_key]   = int32_t{30};
  obj["active"_key] = true;

  std::cout << "name   : " << obj["name"_key].as<std::string>() << "\n";
  std::cout << "age    : " << obj["age"_key].as<int32_t>()      << "\n";
  std::cout << "active : " << obj["active"_key].as<bool>()      << "\n";

  // ── Numeric (array-like) fields ───────────────────────────────────────────
  // Use a size_t index to store ordered sequences inside the same object.
  dynamic list;
  list[0] = std::string{"red"};
  list[1] = std::string{"green"};
  list[2] = std::string{"blue"};
  std::cout << "list size : " << list.size() << "\n";
  std::cout << "list[1]   : " << list[1].as<std::string>() << "\n";

  // clear() removes numeric keys but keeps named fields.
  list["label"_key] = std::string{"colors"};
  list.clear();
  std::cout << "size after clear: " << list.size() << "\n";
  std::cout << "label survives  : " << list["label"_key].as<std::string>() << "\n";

  // ── addField / addMethod guard against duplicates ─────────────────────────
  bool first_add  = obj.addField("score"_key, field{int32_t{100}});
  bool second_add = obj.addField("score"_key, field{int32_t{999}});
  std::cout << "first addField succeeded : " << first_add  << "\n";
  std::cout << "second addField rejected : " << !second_add << "\n";
  std::cout << "score (first value kept): " << obj["score"_key].as<int32_t>() << "\n";

  // ── Nested dynamic objects ────────────────────────────────────────────────
  dynamic_ptr address{0U, {
    {"street"_key, std::string{"123 Main St"}},
    {"city"_key,   std::string{"Springfield"}}
  }};
  obj["address"_key] = std::shared_ptr<dynamic>{address};

  auto addr = obj["address"_key].as<std::shared_ptr<dynamic>>();
  std::cout << "city: " << (*addr)["city"_key].as<std::string>() << "\n";

  // ── Clone ─────────────────────────────────────────────────────────────────
  dynamic copy = obj.clone();
  copy["name"_key] = std::string{"Bob"};
  std::cout << "original name: " << obj["name"_key].as<std::string>() << "\n";
  std::cout << "clone name   : " << copy["name"_key].as<std::string>() << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 4: Binary serialization (standard / self-describing mode)
//
// serialize() writes each key+field pair.  deserialize() reconstructs the
// object without any external schema.
// ─────────────────────────────────────────────────────────────────────────────
static void example_serialization() {
  section("Binary serialization - standard mode");

  dynamic src{"Config"_key};
  src["width"_key]  = int32_t{1920};
  src["height"_key] = int32_t{1080};
  src["title"_key]  = std::string{"My App"};
  src["scale"_key]  = 1.5f;
  // Note: only vector<bool>, vector<int32_t>, and vector<float> are supported.
  src["flags"_key]  = std::vector<bool>{true, false, true};
  src["scores"_key] = std::vector<int32_t>{10, 20, 30};

  // Serialize to an in-memory stream.
  std::stringstream ss;
  { stream_serializer out{ss}; src.serialize(out); }

  std::cout << "Serialized size: " << ss.str().size() << " bytes\n";

  // Deserialize back.
  stream_deserializer in{ss};
  auto dst = dynamic::deserialize(in);
  std::cout << "width  : " << (*dst)["width"_key].as<int32_t>()     << "\n";
  std::cout << "title  : " << (*dst)["title"_key].as<std::string>() << "\n";
  std::cout << "scale  : " << (*dst)["scale"_key].as<float>()       << "\n";
  {
    using vb = std::vector<bool>;
    std::cout << "flags[0]: " << (*dst)["flags"_key].as<vb>()[0]    << "\n";
  }

  // ── Nested object serialization ───────────────────────────────────────────
  dynamic outer;
  outer["meta"_key] = dynamic_ptr{0U, {{"version"_key, int32_t{3}}}};

  std::stringstream ss2;
  { stream_serializer out2{ss2}; outer.serialize(out2); }
  stream_deserializer in2{ss2};
  auto restored = dynamic::deserialize(in2);
  auto meta = (*restored)["meta"_key].as<std::shared_ptr<dynamic>>();
  std::cout << "nested version: " << (*meta)["version"_key].as<int32_t>() << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 5: Template-based serialization (compact mode)
//
// serializeWithTemplate() omits field keys from the wire; only values are
// written.  The receiver must know the class schema in advance (registered
// with addClass).  This produces smaller payloads for known message types.
// ─────────────────────────────────────────────────────────────────────────────
static void example_template_serialization() {
  section("Template serialization - compact mode");

  // Clear any leftover classes from previous examples.
  {
    std::unique_lock<std::shared_mutex> lk(dynamic::getMutex());
    dynamic::getClasses().clear();
  }

  // Register the class prototype.  The prototype defines the field layout.
  dynamic::addClass(
      0U,
      dynamic_ptr{"Vector3"_key, {
        {"x"_key, float{0}},
        {"y"_key, float{0}},
        {"z"_key, float{0}}
      }});

  // Create and populate an instance.
  dynamic v = dynamic::instantiate("Vector3"_key);
  v["x"_key] = float{1.0f};
  v["y"_key] = float{2.0f};
  v["z"_key] = float{3.0f};

  std::stringstream ss;
  { stream_serializer out{ss}; v.serializeWithTemplate(out); }

  std::cout << "Template-serialized size: " << ss.str().size() << " bytes\n";
  // (Compare with self-describing mode which would also write field-name keys.)

  stream_deserializer in{ss};
  auto restored = dynamic::deserializeWithTemplate(in);
  std::cout << "x=" << (*restored)["x"_key].as<float>()
            << " y=" << (*restored)["y"_key].as<float>()
            << " z=" << (*restored)["z"_key].as<float>() << "\n";

  // Clean up
  std::unique_lock<std::shared_mutex> lk(dynamic::getMutex());
  dynamic::getClasses().clear();
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 6: Methods – attaching behaviour to objects
//
// addMethod() registers a std::function on an object.  The function receives
// the object itself (self) and a dynamic holding the call arguments (params),
// and returns a dynamic result.
// ─────────────────────────────────────────────────────────────────────────────
static void example_methods() {
  section("Methods - attaching behaviour to objects");

  dynamic calc{"Calculator"_key};

  // Register an "add" method.
  calc.addMethod("add"_key, [](dynamic& self, const dynamic& params) -> dynamic {
    int32_t a = params["a"_key].as<int32_t>();
    int32_t b = params["b"_key].as<int32_t>();
    dynamic result;
    result["value"_key] = a + b;
    return result;
  });

  // Register an "accumulate" method that mutates internal state via self.
  calc["total"_key] = int32_t{0};
  calc.addMethod("accumulate"_key, [](dynamic& self, const dynamic& params) -> dynamic {
    int32_t n     = params["n"_key].as<int32_t>();
    int32_t total = self["total"_key].as<int32_t>() + n;
    self["total"_key] = total;
    dynamic result;
    result["total"_key] = total;
    return result;
  });

  // Call "add".
  dynamic args;
  args["a"_key] = int32_t{10};
  args["b"_key] = int32_t{32};
  dynamic sum = calc.call("add"_key, args);
  std::cout << "10 + 32 = " << sum["value"_key].as<int32_t>() << "\n";

  // Call "accumulate" several times.
  for (int i = 1; i <= 5; ++i) {
    dynamic p;
    p["n"_key] = int32_t{i};
    calc.call("accumulate"_key, p);
  }
  std::cout << "accumulated total (1+2+3+4+5): "
            << calc["total"_key].as<int32_t>() << "\n";

  // Calling a non-existent method throws.
  try {
    calc.call("sqrt"_key, dynamic{});
  } catch (const std::runtime_error& e) {
    std::cout << "Expected error: " << e.what() << "\n";
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 7: Class hierarchy and inheritance
//
// addClass() registers a prototype in a global registry.  Fields and methods
// defined on parent classes are accessible on child instances without copying
// the prototype data into each instance upfront.
// ─────────────────────────────────────────────────────────────────────────────
static void example_inheritance() {
  section("Class hierarchy and inheritance");

  // Clear any leftover classes from previous examples.
  {
    std::unique_lock<std::shared_mutex> lk(dynamic::getMutex());
    dynamic::getClasses().clear();
  }

  // ── Register a base class ─────────────────────────────────────────────────
  auto shape = dynamic_ptr{"Shape"_key, {{"color"_key, std::string{"black"}}}};
  shape->addMethod("describe"_key, [](dynamic& self, const dynamic& params) -> dynamic {
    dynamic result;
    result["text"_key] = self["color"_key].as<std::string>() + " shape";
    return result;
  });
  dynamic::addClass(0U, shape);

  // ── Register a child class ────────────────────────────────────────────────
  auto circle = dynamic_ptr{"Circle"_key, {{"radius"_key, float{1.0f}}}};
  circle->addMethod("area"_key, [](dynamic& self, const dynamic& params) -> dynamic {
    const float pi = 3.14159265f;
    float r = self["radius"_key].as<float>();
    dynamic result;
    result["area"_key] = pi * r * r;
    return result;
  });
  dynamic::addClass("Shape"_key, circle);

  // ── Instantiate and use ───────────────────────────────────────────────────
  dynamic c = dynamic::instantiate("Circle"_key);
  c["radius"_key] = float{5.0f};
  c["color"_key]  = std::string{"red"};  // field inherited from Shape

  // Call own method.
  dynamic area = c.call("area"_key, dynamic{});
  std::cout << "Circle area (r=5): " << area["area"_key].as<float>() << "\n";

  // Call inherited method from Shape.
  dynamic desc = c.call("describe"_key, dynamic{});
  std::cout << "Description: " << desc["text"_key].as<std::string>() << "\n";

  // inherited field from Shape available on a fresh instance:
  dynamic c2 = dynamic::instantiate("Circle"_key);
  std::cout << "Inherited color: " << c2["color"_key].as<std::string>() << "\n";

  // Circular inheritance is rejected.
  bool ok = dynamic::addClass("Circle"_key, dynamic_ptr{"Shape"_key});
  std::cout << "Circular addClass rejected: " << !ok << "\n";

  // findClass() walks the hierarchy.
  std::cout << "c is-a Shape: " << (c.findClass("Shape"_key) != nullptr) << "\n";

  // Clean up
  std::unique_lock<std::shared_mutex> lk(dynamic::getMutex());
  dynamic::getClasses().clear();
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 8: Userdata – attaching non-serialized context
//
// setUserdata() attaches an arbitrary C++ object to a dynamic instance.
// The data is NOT included in the serialized payload; it is purely for
// runtime application use.
// ─────────────────────────────────────────────────────────────────────────────
static void example_userdata() {
  section("Userdata - attaching non-serialized context");

  class RenderState : public userdata {
   public:
    explicit RenderState(int id) : gpu_handle(id) {}
    int gpu_handle;
  };

  dynamic mesh{"Mesh"_key};
  mesh["vertex_count"_key] = int32_t{1024};

  // Attach the render state to the object.
  mesh.setUserdata(std::make_shared<RenderState>(42));

  // Retrieve it later.
  auto rs = std::dynamic_pointer_cast<RenderState>(mesh.getUserdata());
  if (rs) {
    std::cout << "GPU handle: " << rs->gpu_handle << "\n";
  }

  // Serialize and deserialize – userdata is not included.
  std::stringstream ss;
  { stream_serializer out{ss}; mesh.serialize(out); }
  stream_deserializer in{ss};
  auto restored = dynamic::deserialize(in);

  std::cout << "vertex_count preserved: "
            << (*restored)["vertex_count"_key].as<int32_t>() << "\n";
  std::cout << "userdata after deserialize (should be null): "
            << (restored->getUserdata() == nullptr ? "null" : "present") << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 9: JSON import (extensions::from_json)
//
// Convert a JSON string into a dynamic object for easy interoperability with
// JSON-based data sources.  Types are mapped automatically (int→int32_t,
// float→float, string→string, bool→bool, object→dynamic, array→dynamic
// with numeric indices, null→null shared_ptr<dynamic>).
// ─────────────────────────────────────────────────────────────────────────────
static void example_json() {
  section("JSON import");

  auto obj = extensions::from_json(R"({
    "name":    "Alice",
    "age":     30,
    "score":   9.5,
    "active":  true,
    "tags":    ["c++", "bison", "serialization"],
    "address": {
      "city":  "Springfield",
      "zip":   12345
    }
  })");

  std::cout << "name   : " << (*obj)["name"].as<std::string>()  << "\n";
  std::cout << "age    : " << (*obj)["age"].as<int32_t>()       << "\n";
  std::cout << "active : " << (*obj)["active"].as<bool>()       << "\n";
  std::cout << "score  : " << (*obj)["score"].as<float>()       << "\n";

  // Nested object
  auto addr = (*obj)["address"].as<std::shared_ptr<dynamic>>();
  std::cout << "city   : " << (*addr)["city"].as<std::string>() << "\n";

  // Array stored as numeric-indexed dynamic
  auto tags = (*obj)["tags"].as<std::shared_ptr<dynamic>>();
  std::cout << "tags[0]: " << (*tags)[0].as<std::string>()      << "\n";
  std::cout << "tags[2]: " << (*tags)[2].as<std::string>()      << "\n";
  std::cout << "tag count: " << tags->size()                    << "\n";

  // Fields can be modified after import.
  (*obj)["name"] = std::string{"Bob"};
  std::cout << "updated name: " << (*obj)["name"].as<std::string>() << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 10: YAML import (extensions::from_yaml)
//
// Convert a YAML document into a dynamic object. Mappings become named fields,
// sequences become numeric-indexed dynamic objects, and scalar values are
// parsed into the most specific supported field type.
// ─────────────────────────────────────────────────────────────────────────────
static void example_yaml() {
  section("YAML import");

  auto obj = extensions::from_yaml(R"(
server:
  host: localhost
  port: 8080
debug: true
threshold: 0.75
tags:
  - yaml
  - bison
  - example
)");

  auto server = (*obj)["server"].as<std::shared_ptr<dynamic>>();
  std::cout << "host      : " << (*server)["host"].as<std::string>() << "\n";
  std::cout << "port      : " << (*server)["port"].as<int32_t>() << "\n";
  std::cout << "debug     : " << (*obj)["debug"].as<bool>() << "\n";
  std::cout << "threshold : " << (*obj)["threshold"].as<float>() << "\n";

  auto tags = (*obj)["tags"].as<std::shared_ptr<dynamic>>();
  std::cout << "tags[0]   : " << (*tags)[0].as<std::string>() << "\n";
  std::cout << "tags[2]   : " << (*tags)[2].as<std::string>() << "\n";
  std::cout << "tag count : " << tags->size() << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main() {
  example_hashing();
  example_field();
  example_dynamic_basics();
  example_serialization();
  example_template_serialization();
  example_methods();
  example_inheritance();
  example_userdata();
  example_json();
  example_yaml();

  std::cout << "\nAll examples completed successfully.\n";
  return 0;
}


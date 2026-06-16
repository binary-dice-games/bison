// MIT License © 2025 Binary Dice Games
// examples/bison_abi_example.cpp
//
// Exercises the Bison C ABI (bison_abi.dll / bison_c.h) through the C++ RAII
// wrapper (include/bison_c.hpp, namespace bdg::bison::abi).  Every operation
// goes through the stable C boundary; no C++ internals are shared with the DLL.
//
// Coverage mirrors bison_example.cpp where the C ABI provides equivalent
// functionality.  Features with no C-ABI counterpart are noted in comments:
//   - Binary serialization (standard, schema-driven, buffer) — not in C ABI.
//   - Field attributes and monostate / hash_t / key_t field types — not in C ABI.
//   - Per-object clear, erase, clone, forEach — not in C ABI.
//   - Userdata — not in C ABI.

#include <iostream>
#include <stdexcept>
#include <string>

#include "include/bison_c.hpp"

using namespace bdg::bison::abi;

// ─────────────────────────────────────────────────────────────────────────────
// Helper
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
// bison_key() and the ""_key literal (defined in bison_c.hpp) both compute the
// same 32-bit FNV-1a hash with MSB forced to 1.
// ─────────────────────────────────────────────────────────────────────────────
static void example_hashing() {
  section("Hashing and keys");

  // Compile-time hash via the _key literal.
  constexpr bison_hash k1 = "velocity"_key;

  // Runtime hash via bison_key() — same value.
  bison_hash k2 = bison_key("velocity");
  std::cout << "\"velocity\"_key == bison_key(\"velocity\"): "
            << std::boolalpha << (k1 == k2) << "\n";

  // Named keys always have the high bit set so they cannot collide with small
  // numeric array indices.
  std::cout << "High bit set on named key: "
            << std::boolalpha << bool(k1 & 0x80000000u) << "\n";

  // dynamic::key() is the wrapper's runtime equivalent of bison_key().
  bison_hash k3 = dynamic::key("score");
  std::cout << "dynamic::key(\"score\") == \"score\"_key: "
            << std::boolalpha << (k3 == "score"_key) << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 2: Field type access and type errors
//
// The C ABI exposes typed setters/getters; requesting the wrong type returns
// BISON_ERR_TYPE (the C++ wrapper converts this to std::runtime_error).
//
// Not tested here (no C-ABI equivalent): field attributes, monostate checks,
// hash_t / key_t as stored field values.
// ─────────────────────────────────────────────────────────────────────────────
static void example_field_types() {
  section("Field type access and type errors");

  auto obj = dynamic::create();
  obj.set("score"_key,  int32_t{42});
  obj.set("label"_key,  "hello");
  obj.set("ratio"_key,  1.5f);
  obj.set("active"_key, true);

  std::cout << "int   : " << obj.get<int32_t>("score"_key) << "\n";
  std::cout << "string: " << obj.get<std::string>("label"_key) << "\n";
  std::cout << "float : " << obj.get<float>("ratio"_key) << "\n";
  std::cout << "bool  : " << std::boolalpha
            << obj.get<bool>("active"_key) << "\n";

  // Requesting the wrong type raises BISON_ERR_TYPE → std::runtime_error.
  try {
    obj.get<float>("score"_key);  // score holds int32_t, not float
  } catch (const std::runtime_error& e) {
    std::cout << "Expected type error: " << e.what() << "\n";
  }

  // A null dynamic reference is a valid field value distinct from "no field".
  obj.set("child"_key, dynamic{});  // null handle → null dynamic ref
  auto child = obj.get<dynamic>("child"_key);
  std::cout << "Null object field (valid, handle is null): "
            << !child << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 3: dynamic — named fields, indexed fields, nested objects
//
// Not tested here (no C-ABI equivalent): addField duplicate rejection, clone,
// per-object clear, forEach / erase / empty.
// ─────────────────────────────────────────────────────────────────────────────
static void example_dynamic_basics() {
  section("dynamic - named fields, indexed fields, nested objects");

  // ── Named fields ──────────────────────────────────────────────────────────
  auto obj = dynamic::create("Person"_key);
  obj["name"_key]   = "Alice";
  obj["age"_key]    = int32_t{30};
  obj["active"_key] = true;

  std::cout << "name  : " << obj.get<std::string>("name"_key) << "\n";
  std::cout << "age   : " << obj.get<int32_t>("age"_key) << "\n";
  std::cout << "active: " << std::boolalpha
            << obj.get<bool>("active"_key) << "\n";

  // operator[] proxy implicit conversion also works for reading.
  std::string name_str = obj["name"_key];
  std::cout << "name (operator[]): " << name_str << "\n";

  // ── Numeric (array-like) fields ───────────────────────────────────────────
  auto list = dynamic::create();
  list.set(size_t{0}, "red");
  list.set(size_t{1}, "green");
  list.set(size_t{2}, "blue");

  std::cout << "list size : " << list.size() << "\n";
  std::cout << "list[1]   : " << list.get<std::string>(size_t{1}) << "\n";

  // ── Nested dynamic objects ────────────────────────────────────────────────
  auto address = dynamic::create();
  address.set("street"_key, "123 Main St");
  address.set("city"_key,   "Springfield");

  obj.set("address"_key, address);

  auto addr_back = obj.get<dynamic>("address"_key);
  std::cout << "city: " << addr_back.get<std::string>("city"_key) << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Serialization (standard, schema-driven, buffer) — not exposed by the C ABI.
// Use the C++ bison.hpp API directly for bison_serialize / bison_deserialize.
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// Example 4: Methods
//
// Methods registered via the C++ wrapper store the lambda in a heap-allocated
// MethodCallback; the C ABI is given a plain function pointer + void* user
// context that dispatches to the lambda.
// ─────────────────────────────────────────────────────────────────────────────
static void example_methods() {
  section("Methods");

  auto calc = dynamic::create("Calculator"_key);
  calc.set("total"_key, int32_t{0});

  // "add" — reads two params and writes the sum into result.
  calc.add_method(
      "add"_key,
      [](dynamic& /*self*/, const dynamic& params, dynamic& result) {
        int32_t a = params.get<int32_t>("a"_key);
        int32_t b = params.get<int32_t>("b"_key);
        result.set("value"_key, a + b);
      });

  // "accumulate" — mutates self and writes the running total into result.
  calc.add_method(
      "accumulate"_key,
      [](dynamic& self, const dynamic& params, dynamic& result) {
        int32_t n     = params.get<int32_t>("n"_key);
        int32_t total = self.get<int32_t>("total"_key) + n;
        self.set("total"_key, total);
        result.set("total"_key, total);
      });

  // Call "add".
  auto args = dynamic::create();
  args.set("a"_key, int32_t{10});
  args.set("b"_key, int32_t{32});
  auto sum = calc.call("add"_key, args);
  std::cout << "10 + 32 = " << sum.get<int32_t>("value"_key) << "\n";

  // Call "accumulate" several times.
  for (int i = 1; i <= 5; ++i) {
    auto p = dynamic::create();
    p.set("n"_key, int32_t{i});
    calc.call("accumulate"_key, p);
  }
  std::cout << "accumulated total (1+2+3+4+5): "
            << calc.get<int32_t>("total"_key) << "\n";

  // Calling a non-existent method returns BISON_ERR_NOT_FOUND → throws.
  try {
    auto empty = dynamic::create();
    calc.call("sqrt"_key, empty);
  } catch (const std::runtime_error& e) {
    std::cout << "Expected error: " << e.what() << "\n";
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 5: Class hierarchy and inheritance
// ─────────────────────────────────────────────────────────────────────────────
static void example_inheritance() {
  section("Class hierarchy and inheritance");

  bison_clear_registry();

  // Register a base class (global namespace, no parent).
  auto shape = dynamic::create("Shape"_key);
  shape.set("color"_key, "black");
  shape.add_method(
      "describe"_key,
      [](dynamic& self, const dynamic& /*params*/, dynamic& result) {
        std::string text = self.get<std::string>("color"_key) + " shape";
        result.set("text"_key, text.c_str());
      });
  dynamic::add_class(bison_hash{0}, shape);  // parent=0 → root class

  // Register a child class (global namespace, parent = Shape).
  auto circle = dynamic::create("Circle"_key);
  circle.set("radius"_key, 1.0f);
  circle.add_method(
      "area"_key,
      [](dynamic& self, const dynamic& /*params*/, dynamic& result) {
        const float pi = 3.14159265f;
        float r        = self.get<float>("radius"_key);
        result.set("area"_key, pi * r * r);
      });
  dynamic::add_class("Shape"_key, circle);  // parent = Shape

  // Instantiate and use.
  auto c = dynamic::instantiate("Circle"_key);
  c.set("radius"_key, 5.0f);
  c.set("color"_key, "red");  // field defined on the Shape prototype

  auto area_args = dynamic::create();
  auto area      = c.call("area"_key, area_args);
  std::cout << "Circle area (r=5): " << area.get<float>("area"_key) << "\n";

  auto desc_args = dynamic::create();
  auto desc      = c.call("describe"_key, desc_args);  // method from Shape
  std::cout << "Description: " << desc.get<std::string>("text"_key) << "\n";

  // A fresh instance inherits the default color from the Shape prototype.
  auto c2 = dynamic::instantiate("Circle"_key);
  std::cout << "Inherited color: " << c2.get<std::string>("color"_key) << "\n";

  // find_class returns the registered prototype.
  auto found = dynamic::find_class("Shape"_key);
  std::cout << "find_class(Shape) found: "
            << std::boolalpha << static_cast<bool>(found) << "\n";

  bison_clear_registry();
}

// ─────────────────────────────────────────────────────────────────────────────
// Userdata — not exposed by the C ABI.
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// Example 6: JSON import
// ─────────────────────────────────────────────────────────────────────────────
static void example_json() {
  section("JSON import (bison_from_json)");

  auto obj = dynamic::from_json(R"({
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

  std::cout << "name   : " << obj.get<std::string>("name"_key) << "\n";
  std::cout << "age    : " << obj.get<int32_t>("age"_key) << "\n";
  std::cout << "active : " << std::boolalpha
            << obj.get<bool>("active"_key) << "\n";
  std::cout << "score  : " << obj.get<float>("score"_key) << "\n";

  // Nested object.
  auto addr = obj.get<dynamic>("address"_key);
  std::cout << "city   : " << addr.get<std::string>("city"_key) << "\n";

  // JSON array is stored as a numeric-indexed dynamic.
  auto tags = obj.get<dynamic>("tags"_key);
  std::cout << "tags[0]: " << tags.get<std::string>(size_t{0}) << "\n";
  std::cout << "tags[2]: " << tags.get<std::string>(size_t{2}) << "\n";
  std::cout << "tag count: " << tags.size() << "\n";

  // Mutation after import.
  obj.set("name"_key, "Bob");
  std::cout << "updated name: " << obj.get<std::string>("name"_key) << "\n";

  // A bad JSON string returns NULL → from_json throws.
  try {
    dynamic::from_json("{bad json}");
  } catch (const std::runtime_error& e) {
    std::cout << "Expected parse error: " << e.what() << "\n";
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 7: YAML import
// ─────────────────────────────────────────────────────────────────────────────
static void example_yaml() {
  section("YAML import (bison_from_yaml)");

  auto obj = dynamic::from_yaml(R"(
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

  auto server = obj.get<dynamic>("server"_key);
  std::cout << "host      : " << server.get<std::string>("host"_key) << "\n";
  std::cout << "port      : " << server.get<int32_t>("port"_key) << "\n";
  std::cout << "debug     : " << std::boolalpha
            << obj.get<bool>("debug"_key) << "\n";
  std::cout << "threshold : " << obj.get<float>("threshold"_key) << "\n";

  auto tags = obj.get<dynamic>("tags"_key);
  std::cout << "tags[0]   : " << tags.get<std::string>(size_t{0}) << "\n";
  std::cout << "tags[2]   : " << tags.get<std::string>(size_t{2}) << "\n";
  std::cout << "tag count : " << tags.size() << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 8: Namespaces — class isolation by unit
// ─────────────────────────────────────────────────────────────────────────────
static void example_namespaces() {
  section("Namespaces - class isolation by unit");

  bison_clear_registry();

  // Register the same class name in two different namespaces.
  auto math_table = dynamic::create("table"_key);
  math_table.set("rows"_key, int32_t{0});
  math_table.set("cols"_key, int32_t{0});
  dynamic::add_class("math"_key, math_table, bison_hash{0});  // no parent

  auto ikea_table = dynamic::create("table"_key);
  ikea_table.set("legs"_key,     int32_t{4});
  ikea_table.set("material"_key, "wood");
  dynamic::add_class("ikea"_key, ikea_table, bison_hash{0});  // no parent

  // Instantiate from each namespace.
  auto mt = dynamic::instantiate("math"_key, "table"_key);
  mt.set("rows"_key, int32_t{10});
  mt.set("cols"_key, int32_t{5});

  auto it = dynamic::instantiate("ikea"_key, "table"_key);
  it.set("legs"_key,     int32_t{4});
  it.set("material"_key, "oak");

  std::cout << "math::table rows=" << mt.get<int32_t>("rows"_key)
            << " cols="            << mt.get<int32_t>("cols"_key) << "\n";
  std::cout << "ikea::table legs=" << it.get<int32_t>("legs"_key)
            << " material="        << it.get<std::string>("material"_key) << "\n";

  // Inheritance within a namespace.
  auto base = dynamic::create("Furniture"_key);
  base.set("warranty"_key, int32_t{5});
  dynamic::add_class("ikea"_key, base, bison_hash{0});

  auto sofa_proto = dynamic::create("Sofa"_key);
  sofa_proto.set("seats"_key, int32_t{3});
  dynamic::add_class("ikea"_key, sofa_proto, "Furniture"_key);

  auto sofa = dynamic::instantiate("ikea"_key, "Sofa"_key);
  std::cout << "ikea::Sofa seats="   << sofa.get<int32_t>("seats"_key)
            << " warranty="          << sofa.get<int32_t>("warranty"_key) << "\n";

  bison_clear_registry();
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main() {
  try {
    example_hashing();
    example_field_types();
    example_dynamic_basics();
    example_methods();
    example_inheritance();
    example_json();
    example_yaml();
    example_namespaces();

    std::cout << "\nAll ABI examples completed successfully.\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FATAL: " << e.what() << "\n";
    return 1;
  }
}

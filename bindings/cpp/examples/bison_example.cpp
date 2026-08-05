// MIT License © 2025 Binary Dice Games
// bindings/cpp/examples/bison_example.cpp
//
// Detailed, runnable examples for the header-only C++ ABI binding
// (bindings/cpp/include/bison/dynamic.hpp). Mirrors
// bindings/python/examples/bison_example.py section-for-section, using the
// binding's dynamic::operator[] / "name"_key vocabulary instead of raw
// bison_c.h calls.
//
// Build: this file is wired into the bison_cpp_examples CMake target
// (bindings/cpp/CMakeLists.txt), which links bison_abi.
//
// Run: ./bison_cpp_examples  (from the build directory)

#include "bison/dynamic.hpp"

#include <cstdio>
#include <iostream>

using namespace bdg::bison::abi;

namespace {

int g_section_index = 0;

void section(const char* title) {
  ++g_section_index;
  std::cout << "\n==========================================\n";
  std::cout << "  " << g_section_index << ". " << title << "\n";
  std::cout << "==========================================\n";
}

// ── Example 1: Hashing and keys ─────────────────────────────────────────────

void example_hashing() {
  section("Hashing and keys");
  constexpr hash_t k1 = hash("velocity");
  constexpr hash_t k2 = "velocity"_key;
  std::cout << "hash(\"velocity\") is stable: " << (k1 == k2) << "\n";
  std::cout << "High bit set on named key: " << ((k1 & 0x80000000u) != 0) << "\n";
  std::cout << "\"velocity\" != \"score\": " << (k1 != hash("score")) << "\n";
  static_assert("velocity"_key == hash("velocity"), "\"name\"_key is a compile-time constant");
}

// ── Example 2: Scalar field get / set ───────────────────────────────────────

void example_scalar_fields() {
  section("Scalar field get / set");
  dynamic h{"Person"_key};
  h["name"_key] = std::string{"Alice"};
  h["age"_key] = int32_t{30};
  h["score"_key] = 9.5f;
  h["active"_key] = true;

  std::cout << "name   : " << h["name"_key].as<std::string>() << "\n";
  std::cout << "age    : " << h["age"_key].as<int32_t>() << "\n";
  std::cout << "score  : " << h["score"_key].as<float>() << "\n";
  std::cout << "active : " << h["active"_key].as<bool>() << "\n";
}

// ── Example 3: Nested objects ───────────────────────────────────────────────

void example_nested_objects() {
  section("Nested objects");
  dynamic person{"Person"_key};
  person["name"_key] = std::string{"Alice"};

  dynamic address;
  address["street"_key] = std::string{"123 Main St"};
  address["city"_key] = std::string{"Springfield"};

  person["address"_key] = address; // bison_set_object takes its own reference

  dynamic addr_out = person["address"_key];
  std::cout << "city: " << addr_out["city"_key].as<std::string>() << "\n";
}

// ── Example 4: Numeric (array-like) indexing ────────────────────────────────

void example_numeric_indexing() {
  section("Numeric (array-like) indexing");
  {
    dynamic lst;
    lst[0] = std::string{"red"};
    lst[1] = std::string{"green"};
    lst[2] = std::string{"blue"};
    std::cout << "list size : " << lst.size() << "\n";
    std::cout << "list[1]   : " << lst[1].as<std::string>() << "\n";
  }
  {
    dynamic scores;
    scores[0] = int32_t{10};
    scores[1] = int32_t{20};
    scores[2] = int32_t{30};
    std::cout << "scores[2] : " << scores[2].as<int32_t>() << "\n";

    try {
      scores[0].as<float>(); // type-locked: int slot rejects float reads
      std::cout << "type mismatch at[0]: unexpected (no error raised)\n";
    } catch (const bison_exception& e) {
      std::cout << "type mismatch at[0]: BISON_ERR_TYPE (expected) -> " << e.what() << "\n";
    }
  }
  {
    dynamic fscores;
    fscores[0] = 1.1f;
    fscores[1] = 2.2f;
    std::cout << "fscores[0]: " << fscores[0].as<float>() << "\n";
  }
}

// ── Example 5: Methods — attaching behaviour to objects ─────────────────────

void example_methods() {
  section("Methods - attaching behaviour to objects");
  dynamic calc{"Calculator"_key};
  calc["total"_key] = int32_t{0};

  calc.addMethod("add"_key, [](dynamic&, const dynamic& params, dynamic& result) {
    result["value"_key] = params["a"_key].as<int32_t>() + params["b"_key].as<int32_t>();
  });
  calc.addMethod("accumulate"_key, [](dynamic& self, const dynamic& params, dynamic& result) {
    int32_t total = self["total"_key].as<int32_t>() + params["n"_key].as<int32_t>();
    self["total"_key] = total;
    result["total"_key] = total;
  });

  dynamic add_args;
  add_args["a"_key] = int32_t{10};
  add_args["b"_key] = int32_t{32};
  dynamic total = calc.call("add"_key, add_args);
  std::cout << "10 + 32 = " << total["value"_key].as<int32_t>() << "\n";

  for (int32_t i = 1; i <= 5; ++i) {
    dynamic args;
    args["n"_key] = i;
    calc.call("accumulate"_key, args);
  }
  std::cout << "accumulated total (1+2+3+4+5): " << calc["total"_key].as<int32_t>() << "\n";

  try {
    calc.call("sqrt"_key);
    std::cout << "call unknown method: unexpected (no error raised)\n";
  } catch (const bison_exception& e) {
    std::cout << "call unknown method: BISON_ERR_NOT_FOUND (expected) -> " << e.what() << "\n";
  }
}

// ── Example 6: Class hierarchy and inheritance ──────────────────────────────

void example_inheritance() {
  section("Class hierarchy and inheritance");
  dynamic::clear_registry();

  dynamic shape{"Shape"_key};
  shape["color"_key] = std::string{"black"};
  shape.addMethod("describe"_key, [](dynamic& self, const dynamic&, dynamic& result) {
    result["text"_key] = self["color"_key].as<std::string>() + " shape";
  });
  dynamic::addClass(shape);

  dynamic circle{"Circle"_key};
  circle["radius"_key] = 1.0f;
  circle.addMethod("area"_key, [](dynamic& self, const dynamic&, dynamic& result) {
    float r = self["radius"_key].as<float>();
    result["area"_key] = 3.14159265f * r * r;
  });
  dynamic::addClass(circle, "Shape"_key);

  dynamic c = dynamic::instantiate("Circle"_key);
  c["radius"_key] = 5.0f;
  c["color"_key] = std::string{"red"}; // overrides the inherited default

  dynamic area_result = c.call("area"_key);
  printf("Circle area (r=5): %.4f\n", area_result["area"_key].as<float>());

  dynamic desc_result = c.call("describe"_key);
  std::cout << "Description: " << desc_result["text"_key].as<std::string>() << "\n";

  dynamic c2 = dynamic::instantiate("Circle"_key);
  std::cout << "Inherited color: " << c2["color"_key].as<std::string>() << "\n";

  dynamic dup{"Shape"_key};
  std::cout << "Duplicate addClass rejected: " << !dynamic::addClass(dup) << "\n";

  auto found = dynamic::find_class("Shape"_key);
  std::cout << "Shape found in registry: " << found.has_value() << "\n";

  dynamic::clear_registry();
}

// ── Example 7: Namespaces ───────────────────────────────────────────────────

void example_namespaces() {
  section("Namespaces - class isolation by unit");
  dynamic::clear_registry();

  dynamic math_table{"table"_key};
  math_table["rows"_key] = int32_t{0};
  math_table["cols"_key] = int32_t{0};
  dynamic::addClass("math"_key, math_table);

  dynamic ikea_table{"table"_key};
  ikea_table["legs"_key] = int32_t{4};
  ikea_table["material"_key] = std::string{"wood"};
  dynamic::addClass("ikea"_key, ikea_table);

  std::cout << "Registered 'table' in both 'math' and 'ikea' namespaces\n";

  dynamic mt = dynamic::instantiate("math"_key, "table"_key);
  mt["rows"_key] = int32_t{10};
  mt["cols"_key] = int32_t{5};

  dynamic it = dynamic::instantiate("ikea"_key, "table"_key);
  it["legs"_key] = int32_t{4};
  it["material"_key] = std::string{"oak"};

  std::cout << "math::table rows=" << mt["rows"_key].as<int32_t>() << " cols=" << mt["cols"_key].as<int32_t>() << "\n";
  std::cout << "ikea::table legs=" << it["legs"_key].as<int32_t>()
            << " material=" << it["material"_key].as<std::string>() << "\n";

  dynamic furniture{"Furniture"_key};
  furniture["warranty"_key] = int32_t{5};
  dynamic::addClass("ikea"_key, furniture);

  dynamic sofa{"Sofa"_key};
  sofa["seats"_key] = int32_t{3};
  dynamic::addClass("ikea"_key, sofa, "Furniture"_key);

  dynamic s = dynamic::instantiate("ikea"_key, "Sofa"_key);
  std::cout << "ikea::Sofa seats=" << s["seats"_key].as<int32_t>() << " warranty=" << s["warranty"_key].as<int32_t>()
            << "\n";

  dynamic::clear_registry();
}

// ── Example 8: JSON import ──────────────────────────────────────────────────

void example_json() {
  section("JSON import");
  dynamic obj = dynamic::from_json(R"({
    "name":   "Alice",
    "age":    30,
    "score":  9.5,
    "active": true,
    "address": {"city": "Springfield", "zip": 12345}
  })");
  std::cout << "name   : " << obj["name"_key].as<std::string>() << "\n";
  std::cout << "age    : " << obj["age"_key].as<int32_t>() << "\n";
  std::cout << "active : " << obj["active"_key].as<bool>() << "\n";
  std::cout << "score  : " << obj["score"_key].as<float>() << "\n";

  dynamic addr = obj["address"_key];
  std::cout << "city   : " << addr["city"_key].as<std::string>() << "\n";

  obj["name"_key] = std::string{"Bob"};
  std::cout << "updated name: " << obj["name"_key].as<std::string>() << "\n";
}

// ── Example 9: YAML import ──────────────────────────────────────────────────

void example_yaml() {
  section("YAML import");
  dynamic obj = dynamic::from_yaml(
      "server:\n"
      "  host: localhost\n"
      "  port: 8080\n"
      "debug: true\n"
      "threshold: 0.75\n");

  dynamic server = obj["server"_key];
  std::cout << "host      : " << server["host"_key].as<std::string>() << "\n";
  std::cout << "port      : " << server["port"_key].as<int32_t>() << "\n";
  std::cout << "debug     : " << obj["debug"_key].as<bool>() << "\n";
  printf("threshold : %.2f\n", obj["threshold"_key].as<float>());
}

// ── Example 10: Reference counting and add_ref ──────────────────────────────

void example_ref_counting() {
  section("Reference counting and add_ref");
  dynamic h;
  h["x"_key] = int32_t{42};

  dynamic alias = h.add_ref();
  std::cout << "alias sees x = " << alias["x"_key].as<int32_t>() << "\n";

  alias["x"_key] = int32_t{99};
  std::cout << "original after alias mutate: x = " << h["x"_key].as<int32_t>() << "\n";
}

} // namespace

int main() {
  example_hashing();
  example_scalar_fields();
  example_nested_objects();
  example_numeric_indexing();
  example_methods();
  example_inheritance();
  example_namespaces();
  example_json();
  example_yaml();
  example_ref_counting();
  std::cout << "\nAll examples completed successfully.\n";
  return 0;
}

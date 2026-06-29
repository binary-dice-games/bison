// MIT License © 2025 Binary Dice Games
// examples/bison_abi_example.cpp
//
// Detailed, runnable examples demonstrating every major feature of the Bison
// C ABI (bison_c.h / bison_abi.dll).  Each example is a self-contained
// function.  Run the executable to see the output.
//
// This file intentionally uses only the stable C ABI — no C++ templates or
// internal headers.  Include only "bison_c.h".

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bison_c.h"

// ─────────────────────────────────────────────────────────────────────────────
// Helper: print a separator line for readability
// ─────────────────────────────────────────────────────────────────────────────
static void section(const char* title) {
  static int index = 0;
  printf("\n==========================================\n");
  printf("  %d. %s\n", ++index, title);
  printf("==========================================\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: read a string field into a stack buffer and print it
// ─────────────────────────────────────────────────────────────────────────────
static void print_string_field(bison_handle h, const char* field_name) {
  bison_hash key = bison_key(field_name);
  size_t len = 0;
  bison_get_string(h, key, NULL, 0, &len);
  char buf[256] = {0};
  bison_get_string(h, key, buf, sizeof(buf), NULL);
  printf("%s\n", buf);
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 1: Hashing and keys
//
// bison_key() computes the same FNV-1a hash as the C++ "name"_key literal.
// Pass the returned bison_hash to every field/class/method API.
// ─────────────────────────────────────────────────────────────────────────────
static void example_hashing(void) {
  section("Hashing and keys");

  bison_hash k1 = bison_key("velocity");
  bison_hash k2 = bison_key("velocity");

  printf("bison_key(\"velocity\") is stable: %s\n", (k1 == k2) ? "true" : "false");

  // Named keys have the high bit set so they never collide with numeric
  // array indices (0, 1, 2, …).
  printf("High bit set on named key: %s\n", (k1 & 0x80000000u) ? "true" : "false");

  // Different names produce different hashes.
  bison_hash kscore = bison_key("score");
  printf("\"velocity\" != \"score\": %s\n", (k1 != kscore) ? "true" : "false");
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 2: Scalar field get / set
//
// bison_set_*(h, name, value) stores typed values on any dynamic object.
// bison_get_*(h, name, &out)  retrieves them; returns BISON_ERR_TYPE on
// mismatch.
// ─────────────────────────────────────────────────────────────────────────────
static void example_scalar_fields(void) {
  section("Scalar field get / set");

  bison_handle h = bison_create(bison_key("Person"));

  bison_set_string(h, bison_key("name"), "Alice");
  bison_set_int(h, bison_key("age"), 30);
  bison_set_float(h, bison_key("score"), 9.5f);
  bison_set_bool(h, bison_key("active"), 1);

  // ── Getters ───────────────────────────────────────────────────────────────
  char name[64] = {0};
  bison_get_string(h, bison_key("name"), name, sizeof(name), NULL);
  printf("name   : %s\n", name);

  int32_t age = 0;
  bison_get_int(h, bison_key("age"), &age);
  printf("age    : %d\n", age);

  float score = 0.f;
  bison_get_float(h, bison_key("score"), &score);
  printf("score  : %.1f\n", score);

  int active = 0;
  bison_get_bool(h, bison_key("active"), &active);
  printf("active : %s\n", active ? "true" : "false");

  // ── Type mismatch returns BISON_ERR_TYPE ──────────────────────────────────
  int32_t wrong = 0;
  bison_error err = bison_get_int(h, bison_key("score"), &wrong);
  printf("type mismatch error: %s\n", (err == BISON_ERR_TYPE) ? "BISON_ERR_TYPE (expected)" : "unexpected");

  bison_release(h);
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 3: Nested objects
//
// bison_set_object() stores a child handle inside a parent.  The library
// increments the child's ref-count, so both the caller and the parent share
// the same underlying object.  Release every handle you own.
// ─────────────────────────────────────────────────────────────────────────────
static void example_nested_objects(void) {
  section("Nested objects");

  bison_handle person = bison_create(bison_key("Person"));
  bison_set_string(person, bison_key("name"), "Alice");

  bison_handle address = bison_create(0);
  bison_set_string(address, bison_key("street"), "123 Main St");
  bison_set_string(address, bison_key("city"), "Springfield");

  // Embed address inside person.
  bison_set_object(person, bison_key("address"), address);

  // bison_set_object incremented address's ref-count; we can release our copy.
  bison_release(address);

  // Retrieve the nested object.
  bison_handle addr_out = NULL;
  bison_get_object(person, bison_key("address"), &addr_out);

  char city[64] = {0};
  bison_get_string(addr_out, bison_key("city"), city, sizeof(city), NULL);
  printf("city: %s\n", city);

  bison_release(addr_out);
  bison_release(person);
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 4: Numeric (array-like) indexing
//
// Use bison_set_*_at / bison_get_*_at with a size_t index to store ordered
// sequences inside a dynamic object.  bison_size() returns the count of
// numeric-index entries (= last_index + 1).
// ─────────────────────────────────────────────────────────────────────────────
static void example_numeric_indexing(void) {
  section("Numeric (array-like) indexing");

  bison_handle list = bison_create(0);
  bison_set_string_at(list, 0, "red");
  bison_set_string_at(list, 1, "green");
  bison_set_string_at(list, 2, "blue");

  printf("list size : %zu\n", bison_size(list));

  char item[64] = {0};
  bison_get_string_at(list, 1, item, sizeof(item), NULL);
  printf("list[1]   : %s\n", item);

  // Float-typed array-like storage in a separate object.
  bison_handle scores = bison_create(0);
  bison_set_int_at(scores, 0, 10);
  bison_set_int_at(scores, 1, 20);
  bison_set_int_at(scores, 2, 30);

  int32_t s = 0;
  bison_get_int_at(scores, 2, &s);
  printf("scores[2] : %d\n", s);

  // Fields are type-locked: assigning float to an int slot returns BISON_ERR_TYPE.
  bison_error type_err = bison_set_float_at(scores, 0, 1.1f);
  printf("type mismatch at[0]: %s\n", (type_err == BISON_ERR_TYPE) ? "BISON_ERR_TYPE (expected)" : "unexpected");

  // Use a separate object for float-typed indices.
  bison_handle fscores = bison_create(0);
  bison_set_float_at(fscores, 0, 1.1f);
  bison_set_float_at(fscores, 1, 2.2f);
  float f0 = 0.f;
  bison_get_float_at(fscores, 0, &f0);
  printf("fscores[0]: %.1f\n", f0);
  bison_release(fscores);

  bison_release(scores);
  bison_release(list);
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 5: Methods – attaching behaviour to objects
//
// bison_add_method() registers a C callback on an object.  bison_call()
// invokes it.  The callback receives self, params (arguments), result (output
// object), and an optional user context pointer.
// ─────────────────────────────────────────────────────────────────────────────

// Method implementations are plain C functions (or static C++ functions).
static void method_add(bison_handle self, bison_handle params, bison_handle result, void* user) {
  (void)self;
  (void)user;
  int32_t a = 0, b = 0;
  bison_get_int(params, bison_key("a"), &a);
  bison_get_int(params, bison_key("b"), &b);
  bison_set_int(result, bison_key("value"), a + b);
}

static void method_accumulate(bison_handle self, bison_handle params, bison_handle result, void* user) {
  (void)user;
  int32_t n = 0;
  bison_get_int(params, bison_key("n"), &n);
  int32_t total = 0;
  bison_get_int(self, bison_key("total"), &total);
  total += n;
  bison_set_int(self, bison_key("total"), total);
  bison_set_int(result, bison_key("total"), total);
}

static void example_methods(void) {
  section("Methods - attaching behaviour to objects");

  bison_handle calc = bison_create(bison_key("Calculator"));
  bison_set_int(calc, bison_key("total"), 0);

  bison_add_method(calc, bison_key("add"), method_add, NULL, NULL);
  bison_add_method(calc, bison_key("accumulate"), method_accumulate, NULL, NULL);

  // Call "add".
  bison_handle args = bison_create(0);
  bison_set_int(args, bison_key("a"), 10);
  bison_set_int(args, bison_key("b"), 32);

  bison_handle sum = NULL;
  bison_call(calc, bison_key("add"), args, &sum);

  int32_t value = 0;
  bison_get_int(sum, bison_key("value"), &value);
  printf("10 + 32 = %d\n", value);

  bison_release(sum);
  bison_release(args);

  // Call "accumulate" five times.
  for (int i = 1; i <= 5; ++i) {
    bison_handle p = bison_create(0);
    bison_set_int(p, bison_key("n"), i);
    bison_handle res = NULL;
    bison_call(calc, bison_key("accumulate"), p, &res);
    bison_release(res);
    bison_release(p);
  }

  int32_t total = 0;
  bison_get_int(calc, bison_key("total"), &total);
  printf("accumulated total (1+2+3+4+5): %d\n", total);

  // Calling a non-existent method returns BISON_ERR_NOT_FOUND.
  bison_handle dummy_params = bison_create(0);
  bison_handle dummy_result = NULL;
  bison_error err = bison_call(calc, bison_key("sqrt"), dummy_params, &dummy_result);
  printf("call unknown method: %s\n", (err == BISON_ERR_NOT_FOUND) ? "BISON_ERR_NOT_FOUND (expected)" : "unexpected");
  bison_release(dummy_params);

  bison_release(calc);
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 6: Class hierarchy and inheritance
//
// bison_add_class() registers a prototype in the global registry.
// bison_instantiate() creates a new instance that inherits the prototype's
// fields and methods.  Pass a parent_name hash to establish inheritance.
// ─────────────────────────────────────────────────────────────────────────────

static void method_describe(bison_handle self, bison_handle params, bison_handle result, void* user) {
  (void)params;
  (void)user;
  char color[64] = {0};
  bison_get_string(self, bison_key("color"), color, sizeof(color), NULL);
  char text[80] = {0};
  snprintf(text, sizeof(text), "%s shape", color);
  bison_set_string(result, bison_key("text"), text);
}

static void method_area(bison_handle self, bison_handle params, bison_handle result, void* user) {
  (void)params;
  (void)user;
  float r = 0.f;
  bison_get_float(self, bison_key("radius"), &r);
  bison_set_float(result, bison_key("area"), 3.14159265f * r * r);
}

static void example_inheritance(void) {
  section("Class hierarchy and inheritance");

  bison_clear_registry();

  // ── Register the base "Shape" class ───────────────────────────────────────
  bison_handle shape = bison_create(bison_key("Shape"));
  bison_set_string(shape, bison_key("color"), "black");
  bison_add_method(shape, bison_key("describe"), method_describe, NULL, NULL);
  bison_add_class(0, shape, 0, NULL);
  bison_release(shape);

  // ── Register the child "Circle" class ─────────────────────────────────────
  bison_handle circle = bison_create(bison_key("Circle"));
  bison_set_float(circle, bison_key("radius"), 1.0f);
  bison_add_method(circle, bison_key("area"), method_area, NULL, NULL);
  bison_add_class(0, circle, bison_key("Shape"), NULL);
  bison_release(circle);

  // ── Instantiate and use ───────────────────────────────────────────────────
  bison_handle c = bison_instantiate(0, bison_key("Circle"));
  bison_set_float(c, bison_key("radius"), 5.0f);
  bison_set_string(c, bison_key("color"), "red"); // inherited from Shape

  // Call own method.
  bison_handle area_result = NULL;
  bison_handle empty_params = bison_create(0);
  bison_call(c, bison_key("area"), empty_params, &area_result);
  float area = 0.f;
  bison_get_float(area_result, bison_key("area"), &area);
  printf("Circle area (r=5): %.4f\n", area);
  bison_release(area_result);

  // Call inherited method from Shape.
  bison_handle desc_result = NULL;
  bison_call(c, bison_key("describe"), empty_params, &desc_result);
  char text[128] = {0};
  bison_get_string(desc_result, bison_key("text"), text, sizeof(text), NULL);
  printf("Description: %s\n", text);
  bison_release(desc_result);
  bison_release(empty_params);

  // Inherited field from Shape available on a fresh instance.
  bison_handle c2 = bison_instantiate(0, bison_key("Circle"));
  char inherited_color[64] = {0};
  bison_get_string(c2, bison_key("color"), inherited_color, sizeof(inherited_color), NULL);
  printf("Inherited color: %s\n", inherited_color);
  bison_release(c2);

  // Duplicate registration is rejected (BISON_ERR_DUPLICATE).
  bison_handle dup = bison_create(bison_key("Shape"));
  bison_error dup_err = bison_add_class(0, dup, 0, NULL);
  printf("Duplicate addClass rejected: %s\n", (dup_err == BISON_ERR_DUPLICATE) ? "true" : "false");
  bison_release(dup);

  // bison_find_class looks up the prototype in the registry.
  bison_handle found = bison_find_class(0, bison_key("Shape"));
  printf("Shape found in registry: %s\n", (found != NULL) ? "true" : "false");
  // found is a non-owning view — do NOT release it.

  bison_release(c);
  bison_clear_registry();
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 7: Namespaces – isolating classes by unit
//
// Pass a non-zero ns_name hash to bison_add_class / bison_instantiate to
// register and instantiate classes in a named namespace.  Classes in different
// namespaces may share the same name without collision.  Pass 0 for global.
// ─────────────────────────────────────────────────────────────────────────────
static void example_namespaces(void) {
  section("Namespaces - class isolation by unit");

  bison_clear_registry();

  // "math" namespace: table is a data structure.
  bison_handle math_table = bison_create(bison_key("table"));
  bison_set_int(math_table, bison_key("rows"), 0);
  bison_set_int(math_table, bison_key("cols"), 0);
  bison_add_class(bison_key("math"), math_table, 0, NULL);
  bison_release(math_table);

  // "ikea" namespace: table is a piece of furniture.
  bison_handle ikea_table = bison_create(bison_key("table"));
  bison_set_int(ikea_table, bison_key("legs"), 4);
  bison_set_string(ikea_table, bison_key("material"), "wood");
  bison_add_class(bison_key("ikea"), ikea_table, 0, NULL);
  bison_release(ikea_table);

  printf("Registered 'table' in both 'math' and 'ikea' namespaces\n");

  bison_handle mt = bison_instantiate(bison_key("math"), bison_key("table"));
  bison_set_int(mt, bison_key("rows"), 10);
  bison_set_int(mt, bison_key("cols"), 5);

  bison_handle it = bison_instantiate(bison_key("ikea"), bison_key("table"));
  bison_set_int(it, bison_key("legs"), 4);
  bison_set_string(it, bison_key("material"), "oak");

  int32_t rows = 0, cols = 0;
  bison_get_int(mt, bison_key("rows"), &rows);
  bison_get_int(mt, bison_key("cols"), &cols);
  printf("math::table rows=%d cols=%d\n", rows, cols);

  int32_t legs = 0;
  char material[64] = {0};
  bison_get_int(it, bison_key("legs"), &legs);
  bison_get_string(it, bison_key("material"), material, sizeof(material), NULL);
  printf("ikea::table legs=%d material=%s\n", legs, material);

  // Inheritance within a namespace.
  bison_handle furniture = bison_create(bison_key("Furniture"));
  bison_set_int(furniture, bison_key("warranty"), 5);
  bison_add_class(bison_key("ikea"), furniture, 0, NULL);
  bison_release(furniture);

  bison_handle sofa = bison_create(bison_key("Sofa"));
  bison_set_int(sofa, bison_key("seats"), 3);
  bison_add_class(bison_key("ikea"), sofa, bison_key("Furniture"), NULL);
  bison_release(sofa);

  bison_handle s = bison_instantiate(bison_key("ikea"), bison_key("Sofa"));
  int32_t seats = 0, warranty = 0;
  bison_get_int(s, bison_key("seats"), &seats);
  bison_get_int(s, bison_key("warranty"), &warranty);
  printf("ikea::Sofa seats=%d warranty=%d\n", seats, warranty);

  bison_release(s);
  bison_release(mt);
  bison_release(it);
  bison_clear_registry();
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 8: JSON import
//
// bison_from_json() parses a JSON string and returns a handle to the root
// object.  Types are mapped the same way as extensions::from_json in C++:
// int→int32_t, float→float, string→string, bool→bool,
// object→nested dynamic, array→numeric-indexed dynamic.
// ─────────────────────────────────────────────────────────────────────────────
static void example_json(void) {
  section("JSON import");

  bison_handle obj = bison_from_json(
      "{"
      "\"name\":   \"Alice\","
      "\"age\":    30,"
      "\"score\":  9.5,"
      "\"active\": true,"
      "\"tags\":   [\"c++\", \"bison\", \"serialization\"],"
      "\"address\": {"
      "  \"city\": \"Springfield\","
      "  \"zip\":  12345"
      "}"
      "}");

  char name[64] = {0};
  bison_get_string(obj, bison_key("name"), name, sizeof(name), NULL);
  printf("name   : %s\n", name);

  int32_t age = 0;
  bison_get_int(obj, bison_key("age"), &age);
  printf("age    : %d\n", age);

  int active = 0;
  bison_get_bool(obj, bison_key("active"), &active);
  printf("active : %s\n", active ? "true" : "false");

  float score = 0.f;
  bison_get_float(obj, bison_key("score"), &score);
  printf("score  : %.1f\n", score);

  // Nested object.
  bison_handle addr = NULL;
  bison_get_object(obj, bison_key("address"), &addr);
  char city[64] = {0};
  bison_get_string(addr, bison_key("city"), city, sizeof(city), NULL);
  printf("city   : %s\n", city);
  bison_release(addr);

  // Array stored as numeric-indexed dynamic.
  bison_handle tags = NULL;
  bison_get_object(obj, bison_key("tags"), &tags);
  char tag0[64] = {0}, tag2[64] = {0};
  bison_get_string_at(tags, 0, tag0, sizeof(tag0), NULL);
  bison_get_string_at(tags, 2, tag2, sizeof(tag2), NULL);
  printf("tags[0]: %s\n", tag0);
  printf("tags[2]: %s\n", tag2);
  printf("tag count: %zu\n", bison_size(tags));
  bison_release(tags);

  // Fields can be modified after import.
  bison_set_string(obj, bison_key("name"), "Bob");
  bison_get_string(obj, bison_key("name"), name, sizeof(name), NULL);
  printf("updated name: %s\n", name);

  bison_release(obj);
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 9: YAML import
//
// bison_from_yaml() parses a YAML document and returns a handle to the root
// object.  The type-coercion rules mirror extensions::from_yaml in C++.
// ─────────────────────────────────────────────────────────────────────────────
static void example_yaml(void) {
  section("YAML import");

  bison_handle obj = bison_from_yaml(
      "server:\n"
      "  host: localhost\n"
      "  port: 8080\n"
      "debug: true\n"
      "threshold: 0.75\n"
      "tags:\n"
      "  - yaml\n"
      "  - bison\n"
      "  - example\n");

  // Nested mapping.
  bison_handle server = NULL;
  bison_get_object(obj, bison_key("server"), &server);
  char host[64] = {0};
  bison_get_string(server, bison_key("host"), host, sizeof(host), NULL);
  printf("host      : %s\n", host);
  int32_t port = 0;
  bison_get_int(server, bison_key("port"), &port);
  printf("port      : %d\n", port);
  bison_release(server);

  int debug = 0;
  bison_get_bool(obj, bison_key("debug"), &debug);
  printf("debug     : %s\n", debug ? "true" : "false");

  float threshold = 0.f;
  bison_get_float(obj, bison_key("threshold"), &threshold);
  printf("threshold : %.2f\n", threshold);

  bison_handle tags = NULL;
  bison_get_object(obj, bison_key("tags"), &tags);
  char tag0[64] = {0}, tag2[64] = {0};
  bison_get_string_at(tags, 0, tag0, sizeof(tag0), NULL);
  bison_get_string_at(tags, 2, tag2, sizeof(tag2), NULL);
  printf("tags[0]   : %s\n", tag0);
  printf("tags[2]   : %s\n", tag2);
  printf("tag count : %zu\n", bison_size(tags));
  bison_release(tags);

  bison_release(obj);
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 10: Reference counting and bison_add_ref
//
// Every creation function returns a handle with ref-count 1.  bison_add_ref()
// creates an additional alias to the same underlying object.  The object is
// destroyed only when all handles have been released.
// ─────────────────────────────────────────────────────────────────────────────
static void example_ref_counting(void) {
  section("Reference counting and bison_add_ref");

  bison_handle h = bison_create(0);
  bison_set_int(h, bison_key("x"), 42);

  // Take a second reference.
  bison_handle alias = bison_add_ref(h);

  // Both handles share the same data.
  int32_t v = 0;
  bison_get_int(alias, bison_key("x"), &v);
  printf("alias sees x = %d\n", v);

  // Mutate through the alias; the original reflects the change.
  bison_set_int(alias, bison_key("x"), 99);
  bison_get_int(h, bison_key("x"), &v);
  printf("original after alias mutate: x = %d\n", v);

  // Release the alias; object survives because h still holds a reference.
  bison_release(alias);

  // Still accessible through h.
  bison_get_int(h, bison_key("x"), &v);
  printf("original after alias release: x = %d\n", v);

  bison_release(h);
  // Object is now destroyed.
  printf("Both handles released\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main(void) {
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

  printf("\nAll examples completed successfully.\n");
  return 0;
}

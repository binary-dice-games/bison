// MIT License © 2025 Binary Dice Games
// Google Test suite for the pure-C Bison shared-library API.

#include "bison_c.h"

#include "src/bison/bison.hpp"

#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <string>

// glibc's <sys/types.h> (pulled in transitively by <gtest/gtest.h>) also
// defines a `key_t` typedef -- alias bdg::bison::key_t under a different
// name here rather than risk an ambiguous bare `key_t` (see wish/CLAUDE.md's
// note on this exact pitfall).
using bison_key_t = bdg::bison::key_t;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

/** RAII wrapper so handles are always released even when a test fails early. */
struct ScopedHandle {
  bison_handle h;
  explicit ScopedHandle(bison_handle h) : h(h) {}
  ~ScopedHandle() {
    bison_release(h);
  }
  operator bison_handle() const {
    return h;
  }
};

static bison_hash H(const char* name) {
  return bison_key(name);
}

static void free_and_null(char** s) {
  bison_free_string(*s);
  *s = nullptr;
}

// ═════════════════════════════════════════════════════════════════════════════
// 1. Lifecycle: create / add_ref / release
// ═════════════════════════════════════════════════════════════════════════════

TEST(LifecycleTests, CreateReturnsNonNull) {
  ScopedHandle h{bison_create(0)};
  EXPECT_NE(h.h, nullptr);
}

TEST(LifecycleTests, InstantiateReturnsNonNull) {
  ScopedHandle h{bison_instantiate(0, 0)};
  EXPECT_NE(h.h, nullptr);
}

namespace {
// A dynamic subclass registered with a factory (mirrors rmi_tests.cpp's
// marked_widget/InstantiateHonorsRegisteredFactory) -- proves
// bison_instantiate() goes through the registered factory instead of
// building a plain, sliced `dynamic` that a caller's dynamic_cast<T*> would
// never recognize as this subclass.
class bison_c_marked_widget : public bdg::bison::dynamic {
 public:
  explicit bison_c_marked_widget(bdg::bison::dynamic&& d) : dynamic(std::move(d)) {
    (*this)[bison_key_t{"marker"}] = std::string{"factory"};
  }
};
} // namespace

TEST(LifecycleTests, InstantiateHonorsRegisteredFactory) {
  auto proto = bdg::bison::dynamic_ptr{bison_key_t{"BisonCMarkedWidget"}, {}};
  bdg::bison::dynamic::addClass(
      bison_key_t{0U},
      proto,
      bison_key_t{0U},
      bdg::bison::dynamic::make_factory<bison_c_marked_widget>(bison_key_t{0U}, bison_key_t{"BisonCMarkedWidget"}));

  ScopedHandle h{bison_instantiate(0, H("BisonCMarkedWidget"))};
  ASSERT_NE(h.h, nullptr);

  char buf[32] = {};
  size_t len = 0;
  EXPECT_EQ(bison_get_string(h, H("marker"), buf, sizeof buf, &len), BISON_OK);
  EXPECT_STREQ(buf, "factory");
}

TEST(LifecycleTests, AddRefReturnsDistinctHandle) {
  ScopedHandle h1{bison_create(0)};
  ASSERT_NE(h1.h, nullptr);
  ScopedHandle h2{bison_add_ref(h1)};
  EXPECT_NE(h2.h, nullptr);
  EXPECT_NE(h1.h, h2.h); // different heap-allocated shared_ptr wrappers
}

TEST(LifecycleTests, AddRefSharesObject) {
  ScopedHandle h1{bison_create(0)};
  ASSERT_NE(h1.h, nullptr);
  EXPECT_EQ(bison_set_int(h1, H("x"), 7), BISON_OK);

  ScopedHandle h2{bison_add_ref(h1)};
  int32_t v = 0;
  EXPECT_EQ(bison_get_int(h2, H("x"), &v), BISON_OK);
  EXPECT_EQ(v, 7);
}

TEST(LifecycleTests, ReleaseNullIsSafe) {
  bison_release(nullptr); // must not crash
}

TEST(LifecycleTests, AddRefOnNullReturnsNull) {
  EXPECT_EQ(bison_add_ref(nullptr), nullptr);
}

TEST(LifecycleTests, CloneOnNullReturnsNull) {
  EXPECT_EQ(bison_clone(nullptr), nullptr);
}

TEST(LifecycleTests, CloneReturnsDistinctHandle) {
  ScopedHandle orig{bison_create(0)};
  ASSERT_NE(orig.h, nullptr);
  ScopedHandle copy{bison_clone(orig)};
  ASSERT_NE(copy.h, nullptr);
  EXPECT_NE(orig.h, copy.h);
}

TEST(LifecycleTests, CloneScalarFieldIsIndependent) {
  ScopedHandle orig{bison_create(0)};
  ASSERT_EQ(bison_set_int(orig, H("v"), 1), BISON_OK);

  ScopedHandle copy{bison_clone(orig)};
  ASSERT_NE(copy.h, nullptr);

  // Mutating the clone must not affect the original.
  ASSERT_EQ(bison_set_int(copy, H("v"), 99), BISON_OK);
  int32_t v = 0;
  ASSERT_EQ(bison_get_int(orig, H("v"), &v), BISON_OK);
  EXPECT_EQ(v, 1);
}

TEST(LifecycleTests, CloneOriginalDoesNotAffectClone) {
  ScopedHandle orig{bison_create(0)};
  ASSERT_EQ(bison_set_int(orig, H("v"), 1), BISON_OK);

  ScopedHandle copy{bison_clone(orig)};
  ASSERT_NE(copy.h, nullptr);

  // Mutating the original must not affect the clone.
  ASSERT_EQ(bison_set_int(orig, H("v"), 42), BISON_OK);
  int32_t v = 0;
  ASSERT_EQ(bison_get_int(copy, H("v"), &v), BISON_OK);
  EXPECT_EQ(v, 1);
}

TEST(LifecycleTests, CloneNestedObjectIsDeep) {
  ScopedHandle inner{bison_create(0)};
  ASSERT_EQ(bison_set_int(inner, H("x"), 10), BISON_OK);

  ScopedHandle orig{bison_create(0)};
  ASSERT_EQ(bison_set_object(orig, H("child"), inner), BISON_OK);

  ScopedHandle copy{bison_clone(orig)};
  ASSERT_NE(copy.h, nullptr);

  // Mutate the nested object through the clone; the original must be unchanged.
  bison_handle raw_child = nullptr;
  ASSERT_EQ(bison_get_object(copy, H("child"), &raw_child), BISON_OK);
  ScopedHandle cloned_child{raw_child};
  ASSERT_EQ(bison_set_int(cloned_child, H("x"), 99), BISON_OK);

  int32_t v = 0;
  ASSERT_EQ(bison_get_int(inner, H("x"), &v), BISON_OK);
  EXPECT_EQ(v, 10);
}

// ═════════════════════════════════════════════════════════════════════════════
// 2. Setters and getters — named fields
// ═════════════════════════════════════════════════════════════════════════════

TEST(SetGetTests, IntRoundTrip) {
  ScopedHandle h{bison_create(0)};
  EXPECT_EQ(bison_set_int(h, H("score"), 42), BISON_OK);
  int32_t v = 0;
  EXPECT_EQ(bison_get_int(h, H("score"), &v), BISON_OK);
  EXPECT_EQ(v, 42);
}

TEST(SetGetTests, FloatRoundTrip) {
  ScopedHandle h{bison_create(0)};
  EXPECT_EQ(bison_set_float(h, H("ratio"), 3.14f), BISON_OK);
  float v = 0.0f;
  EXPECT_EQ(bison_get_float(h, H("ratio"), &v), BISON_OK);
  EXPECT_NEAR(v, 3.14f, 1e-4f);
}

TEST(SetGetTests, KeyRoundTrip) {
  ScopedHandle h{bison_create(0)};
  EXPECT_EQ(bison_set_key(h, H("id"), H("Topdown")), BISON_OK);
  bison_hash v = 0;
  EXPECT_EQ(bison_get_key(h, H("id"), &v), BISON_OK);
  EXPECT_EQ(v, H("Topdown"));
}

TEST(SetGetTests, KeyDoesNotAliasInt) {
  ScopedHandle h{bison_create(0)};
  bison_set_int(h, H("n"), 1);
  bison_hash v = 0;
  EXPECT_EQ(bison_get_key(h, H("n"), &v), BISON_ERR_TYPE);
}

TEST(SetGetTests, BoolRoundTrip) {
  ScopedHandle h{bison_create(0)};
  EXPECT_EQ(bison_set_bool(h, H("flag"), 1), BISON_OK);
  int v = 0;
  EXPECT_EQ(bison_get_bool(h, H("flag"), &v), BISON_OK);
  EXPECT_EQ(v, 1);
}

TEST(SetGetTests, StringRoundTrip) {
  ScopedHandle h{bison_create(0)};
  EXPECT_EQ(bison_set_string(h, H("name"), "alice"), BISON_OK);
  char buf[32] = {};
  size_t len = 0;
  EXPECT_EQ(bison_get_string(h, H("name"), buf, sizeof buf, &len), BISON_OK);
  EXPECT_STREQ(buf, "alice");
  EXPECT_EQ(len, 5u);
}

TEST(SetGetTests, StringQueryLengthWithNullBuf) {
  ScopedHandle h{bison_create(0)};
  bison_set_string(h, H("msg"), "hello");
  size_t len = 0;
  EXPECT_EQ(bison_get_string(h, H("msg"), nullptr, 0, &len), BISON_OK);
  EXPECT_EQ(len, 5u);
}

TEST(SetGetTests, NestedObjectRoundTrip) {
  ScopedHandle parent{bison_create(0)};
  ScopedHandle child{bison_create(0)};
  bison_set_int(child, H("x"), 99);

  EXPECT_EQ(bison_set_object(parent, H("inner"), child), BISON_OK);

  bison_handle retrieved = nullptr;
  EXPECT_EQ(bison_get_object(parent, H("inner"), &retrieved), BISON_OK);
  ASSERT_NE(retrieved, nullptr);
  int32_t v = 0;
  bison_get_int(retrieved, H("x"), &v);
  EXPECT_EQ(v, 99);
  bison_release(retrieved);
}

TEST(SetGetTests, SetNullObject) {
  ScopedHandle h{bison_create(0)};
  // First set a real object so the field type is set.
  ScopedHandle child{bison_create(0)};
  bison_set_object(h, H("ref"), child);
  // Now set it to null.
  EXPECT_EQ(bison_set_object(h, H("ref"), nullptr), BISON_OK);
}

TEST(SetGetTests, WrongTypeReturnsTypeError) {
  ScopedHandle h{bison_create(0)};
  bison_set_int(h, H("n"), 1);
  float v = 0;
  EXPECT_EQ(bison_get_float(h, H("n"), &v), BISON_ERR_TYPE);
}

TEST(SetGetTests, KeyNullHandleReturnsNullError) {
  bison_hash v = 0;
  EXPECT_EQ(bison_get_key(nullptr, H("x"), &v), BISON_ERR_NULL);
  EXPECT_EQ(bison_set_key(nullptr, H("x"), H("y")), BISON_ERR_NULL);
}

TEST(SetGetTests, NullHandleReturnsNullError) {
  int32_t v = 0;
  EXPECT_EQ(bison_get_int(nullptr, H("x"), &v), BISON_ERR_NULL);
  EXPECT_EQ(bison_set_int(nullptr, H("x"), 1), BISON_ERR_NULL);
}

// ═════════════════════════════════════════════════════════════════════════════
// 2b. Vector field registration
// ═════════════════════════════════════════════════════════════════════════════

TEST(VectorFieldTests, BoolVectorRoundTripsThroughJson) {
  ScopedHandle h{bison_create(0)};
  const int values[] = {1, 0, 1};
  ASSERT_EQ(bison_add_field_vector_bool(h, H("flags"), values, 3, nullptr), BISON_OK);

  char* out = nullptr;
  ASSERT_EQ(bison_to_json(h, -1, &out), BISON_OK);
  ASSERT_NE(out, nullptr);
  EXPECT_NE(std::string{out}.find("[true,false,true]"), std::string::npos);
  free_and_null(&out);
}

TEST(VectorFieldTests, IntVectorRoundTripsThroughJson) {
  ScopedHandle h{bison_create(0)};
  const int32_t values[] = {1, 2, 3};
  ASSERT_EQ(bison_add_field_vector_int(h, H("scores"), values, 3, nullptr), BISON_OK);

  char* out = nullptr;
  ASSERT_EQ(bison_to_json(h, -1, &out), BISON_OK);
  ASSERT_NE(out, nullptr);
  EXPECT_NE(std::string{out}.find("[1,2,3]"), std::string::npos);
  free_and_null(&out);
}

TEST(VectorFieldTests, FloatVectorRoundTripsThroughJson) {
  ScopedHandle h{bison_create(0)};
  const float values[] = {1.5f, 2.5f};
  ASSERT_EQ(bison_add_field_vector_float(h, H("ratios"), values, 2, nullptr), BISON_OK);

  char* out = nullptr;
  ASSERT_EQ(bison_to_json(h, -1, &out), BISON_OK);
  ASSERT_NE(out, nullptr);
  EXPECT_NE(std::string{out}.find("1.5"), std::string::npos);
  EXPECT_NE(std::string{out}.find("2.5"), std::string::npos);
  free_and_null(&out);
}

TEST(VectorFieldTests, BytesVectorRoundTripsThroughJson) {
  ScopedHandle h{bison_create(0)};
  const uint8_t values[] = {0, 1, 255};
  ASSERT_EQ(bison_add_field_vector_bytes(h, H("blob"), values, 3, nullptr), BISON_OK);

  char* out = nullptr;
  ASSERT_EQ(bison_to_json(h, -1, &out), BISON_OK);
  ASSERT_NE(out, nullptr);
  free_and_null(&out);
}

TEST(VectorFieldTests, EmptyVectorWithNullValuesSucceeds) {
  ScopedHandle h{bison_create(0)};
  EXPECT_EQ(bison_add_field_vector_int(h, H("empty"), nullptr, 0, nullptr), BISON_OK);
  EXPECT_EQ(bison_add_field_vector_bool(h, H("empty_b"), nullptr, 0, nullptr), BISON_OK);
  EXPECT_EQ(bison_add_field_vector_float(h, H("empty_f"), nullptr, 0, nullptr), BISON_OK);
  EXPECT_EQ(bison_add_field_vector_bytes(h, H("empty_y"), nullptr, 0, nullptr), BISON_OK);
}

TEST(VectorFieldTests, DuplicateKeyReturnsDuplicateError) {
  ScopedHandle h{bison_create(0)};
  const int32_t values[] = {1};
  ASSERT_EQ(bison_add_field_vector_int(h, H("dup"), values, 1, nullptr), BISON_OK);
  EXPECT_EQ(bison_add_field_vector_int(h, H("dup"), values, 1, nullptr), BISON_ERR_DUPLICATE);
}

TEST(VectorFieldTests, NullObjectHandleReturnsNullError) {
  const int32_t values[] = {1};
  EXPECT_EQ(bison_add_field_vector_int(nullptr, H("x"), values, 1, nullptr), BISON_ERR_NULL);
  EXPECT_EQ(bison_add_field_vector_bool(nullptr, H("x"), nullptr, 0, nullptr), BISON_ERR_NULL);
  EXPECT_EQ(bison_add_field_vector_float(nullptr, H("x"), nullptr, 0, nullptr), BISON_ERR_NULL);
  EXPECT_EQ(bison_add_field_vector_bytes(nullptr, H("x"), nullptr, 0, nullptr), BISON_ERR_NULL);
}

// ═════════════════════════════════════════════════════════════════════════════
// 2c. Vector field read-back / mutation
// ═════════════════════════════════════════════════════════════════════════════

TEST(VectorFieldAccessTests, IntSetThenGetRoundTrip) {
  ScopedHandle h{bison_create(0)};
  const int32_t values[] = {1, 2, 3};
  ASSERT_EQ(bison_set_vector_int(h, H("scores"), values, 3), BISON_OK);

  size_t len = 0;
  ASSERT_EQ(bison_get_vector_int(h, H("scores"), nullptr, 0, &len), BISON_OK);
  ASSERT_EQ(len, 3u);

  int32_t out[3] = {};
  ASSERT_EQ(bison_get_vector_int(h, H("scores"), out, 3, nullptr), BISON_OK);
  EXPECT_EQ(out[0], 1);
  EXPECT_EQ(out[1], 2);
  EXPECT_EQ(out[2], 3);
}

TEST(VectorFieldAccessTests, RegisteredVectorIsReadable) {
  // The gap this closes: bison_add_field_vector_int() alone (no
  // bison_get_vector_int()) previously left a vector field write-only.
  ScopedHandle h{bison_create(0)};
  const int32_t values[] = {10, 20};
  ASSERT_EQ(bison_add_field_vector_int(h, H("v"), values, 2, nullptr), BISON_OK);

  int32_t out[2] = {};
  size_t len = 0;
  ASSERT_EQ(bison_get_vector_int(h, H("v"), out, 2, &len), BISON_OK);
  EXPECT_EQ(len, 2u);
  EXPECT_EQ(out[0], 10);
  EXPECT_EQ(out[1], 20);
}

TEST(VectorFieldAccessTests, BoolSetThenGetRoundTrip) {
  ScopedHandle h{bison_create(0)};
  const int values[] = {1, 0, 1};
  ASSERT_EQ(bison_set_vector_bool(h, H("flags"), values, 3), BISON_OK);

  int out[3] = {};
  size_t len = 0;
  ASSERT_EQ(bison_get_vector_bool(h, H("flags"), out, 3, &len), BISON_OK);
  ASSERT_EQ(len, 3u);
  EXPECT_EQ(out[0], 1);
  EXPECT_EQ(out[1], 0);
  EXPECT_EQ(out[2], 1);
}

TEST(VectorFieldAccessTests, FloatSetThenGetRoundTrip) {
  ScopedHandle h{bison_create(0)};
  const float values[] = {1.5f, 2.5f};
  ASSERT_EQ(bison_set_vector_float(h, H("ratios"), values, 2), BISON_OK);

  float out[2] = {};
  ASSERT_EQ(bison_get_vector_float(h, H("ratios"), out, 2, nullptr), BISON_OK);
  EXPECT_NEAR(out[0], 1.5f, 1e-4f);
  EXPECT_NEAR(out[1], 2.5f, 1e-4f);
}

TEST(VectorFieldAccessTests, BytesSetThenGetRoundTrip) {
  ScopedHandle h{bison_create(0)};
  const uint8_t values[] = {0, 1, 255};
  ASSERT_EQ(bison_set_vector_bytes(h, H("blob"), values, 3), BISON_OK);

  uint8_t out[3] = {};
  ASSERT_EQ(bison_get_vector_bytes(h, H("blob"), out, 3, nullptr), BISON_OK);
  EXPECT_EQ(out[0], 0);
  EXPECT_EQ(out[1], 1);
  EXPECT_EQ(out[2], 255);
}

TEST(VectorFieldAccessTests, SetReplacesExistingContents) {
  ScopedHandle h{bison_create(0)};
  const int32_t first[] = {1, 2, 3};
  const int32_t second[] = {9, 9};
  ASSERT_EQ(bison_set_vector_int(h, H("v"), first, 3), BISON_OK);
  ASSERT_EQ(bison_set_vector_int(h, H("v"), second, 2), BISON_OK);

  int32_t out[2] = {};
  size_t len = 0;
  ASSERT_EQ(bison_get_vector_int(h, H("v"), out, 2, &len), BISON_OK);
  EXPECT_EQ(len, 2u);
  EXPECT_EQ(out[0], 9);
  EXPECT_EQ(out[1], 9);
}

TEST(VectorFieldAccessTests, GetWithNullBufOnlyQueriesLength) {
  ScopedHandle h{bison_create(0)};
  const int32_t values[] = {1, 2, 3, 4};
  ASSERT_EQ(bison_set_vector_int(h, H("v"), values, 4), BISON_OK);

  size_t len = 0;
  EXPECT_EQ(bison_get_vector_int(h, H("v"), nullptr, 0, &len), BISON_OK);
  EXPECT_EQ(len, 4u);
}

TEST(VectorFieldAccessTests, GetWrongTypeReturnsTypeError) {
  ScopedHandle h{bison_create(0)};
  ASSERT_EQ(bison_set_int(h, H("x"), 1), BISON_OK);
  int32_t out[1] = {};
  EXPECT_EQ(bison_get_vector_int(h, H("x"), out, 1, nullptr), BISON_ERR_TYPE);
}

TEST(VectorFieldAccessTests, SetWrongTypeReturnsTypeError) {
  ScopedHandle h{bison_create(0)};
  ASSERT_EQ(bison_set_int(h, H("x"), 1), BISON_OK);
  const int32_t values[] = {1};
  EXPECT_EQ(bison_set_vector_int(h, H("x"), values, 1), BISON_ERR_TYPE);
}

TEST(VectorFieldAccessTests, EmptyVectorWithNullValuesSucceeds) {
  ScopedHandle h{bison_create(0)};
  EXPECT_EQ(bison_set_vector_int(h, H("empty"), nullptr, 0), BISON_OK);
  size_t len = 1; // must be overwritten with 0
  EXPECT_EQ(bison_get_vector_int(h, H("empty"), nullptr, 0, &len), BISON_OK);
  EXPECT_EQ(len, 0u);
}

TEST(VectorFieldAccessTests, NullHandleReturnsNullError) {
  const int32_t values[] = {1};
  int32_t out[1] = {};
  EXPECT_EQ(bison_set_vector_int(nullptr, H("x"), values, 1), BISON_ERR_NULL);
  EXPECT_EQ(bison_get_vector_int(nullptr, H("x"), out, 1, nullptr), BISON_ERR_NULL);
}

// ═════════════════════════════════════════════════════════════════════════════
// 3. Indexed (array-like) access
// ═════════════════════════════════════════════════════════════════════════════

TEST(IndexedTests, IntAtRoundTrip) {
  ScopedHandle h{bison_create(0)};
  EXPECT_EQ(bison_set_int_at(h, 0, 10), BISON_OK);
  EXPECT_EQ(bison_set_int_at(h, 1, 20), BISON_OK);
  EXPECT_EQ(bison_set_int_at(h, 2, 30), BISON_OK);
  EXPECT_EQ(bison_size(h), 3u);
  int32_t v = 0;
  EXPECT_EQ(bison_get_int_at(h, 1, &v), BISON_OK);
  EXPECT_EQ(v, 20);
}

TEST(IndexedTests, FloatAtRoundTrip) {
  ScopedHandle h{bison_create(0)};
  bison_set_float_at(h, 0, 1.5f);
  float v = 0;
  EXPECT_EQ(bison_get_float_at(h, 0, &v), BISON_OK);
  EXPECT_NEAR(v, 1.5f, 1e-4f);
}

TEST(IndexedTests, StringAtRoundTrip) {
  ScopedHandle h{bison_create(0)};
  bison_set_string_at(h, 0, "item0");
  char buf[32] = {};
  EXPECT_EQ(bison_get_string_at(h, 0, buf, sizeof buf, nullptr), BISON_OK);
  EXPECT_STREQ(buf, "item0");
}

TEST(IndexedTests, BoolAtRoundTrip) {
  ScopedHandle h{bison_create(0)};
  EXPECT_EQ(bison_set_bool_at(h, 0, 1), BISON_OK);
  EXPECT_EQ(bison_set_bool_at(h, 1, 0), BISON_OK);
  int v = -1;
  EXPECT_EQ(bison_get_bool_at(h, 0, &v), BISON_OK);
  EXPECT_EQ(v, 1);
  EXPECT_EQ(bison_get_bool_at(h, 1, &v), BISON_OK);
  EXPECT_EQ(v, 0);
}

TEST(IndexedTests, KeyAtRoundTrip) {
  ScopedHandle h{bison_create(0)};
  EXPECT_EQ(bison_set_key_at(h, 0, H("hero")), BISON_OK);
  bison_hash v = 0;
  EXPECT_EQ(bison_get_key_at(h, 0, &v), BISON_OK);
  EXPECT_EQ(v, H("hero"));
}

TEST(IndexedTests, KeyAtDoesNotAliasIntAt) {
  ScopedHandle h{bison_create(0)};
  ASSERT_EQ(bison_set_int_at(h, 0, 42), BISON_OK);
  EXPECT_EQ(bison_set_key_at(h, 0, H("anything")), BISON_ERR_TYPE);
}

TEST(IndexedTests, ObjectAtRoundTrip) {
  ScopedHandle h{bison_create(0)};
  ScopedHandle child{bison_create(0)};
  ASSERT_EQ(bison_set_int(child, H("v"), 7), BISON_OK);

  EXPECT_EQ(bison_set_object_at(h, 0, child), BISON_OK);
  bison_handle out = nullptr;
  EXPECT_EQ(bison_get_object_at(h, 0, &out), BISON_OK);
  ASSERT_NE(out, nullptr);
  int32_t v = 0;
  EXPECT_EQ(bison_get_int(out, H("v"), &v), BISON_OK);
  EXPECT_EQ(v, 7);
  bison_release(out);
}

TEST(IndexedTests, ObjectAtNullRoundTrip) {
  ScopedHandle h{bison_create(0)};
  EXPECT_EQ(bison_set_object_at(h, 0, nullptr), BISON_OK);
  bison_handle out = reinterpret_cast<bison_handle>(1); // sentinel, must be overwritten with NULL
  EXPECT_EQ(bison_get_object_at(h, 0, &out), BISON_OK);
  EXPECT_EQ(out, nullptr);
}

TEST(IndexedTests, SizeOfEmptyObjectIsZero) {
  ScopedHandle h{bison_create(0)};
  EXPECT_EQ(bison_size(h), 0u);
}

TEST(IndexedTests, SizeOfNullIsZero) {
  EXPECT_EQ(bison_size(nullptr), 0u);
}

// ═════════════════════════════════════════════════════════════════════════════
// 3b. Binary serialization
// ═════════════════════════════════════════════════════════════════════════════

TEST(SerializationTests, RoundTripsScalarFields) {
  ScopedHandle h{bison_create(0)};
  ASSERT_EQ(bison_set_int(h, H("x"), 42), BISON_OK);
  ASSERT_EQ(bison_set_float(h, H("y"), 2.5f), BISON_OK);
  ASSERT_EQ(bison_set_bool(h, H("z"), 1), BISON_OK);
  ASSERT_EQ(bison_set_string(h, H("s"), "hello"), BISON_OK);

  uint8_t* buf = nullptr;
  size_t len = 0;
  ASSERT_EQ(bison_serialize(h, &buf, &len), BISON_OK);
  ASSERT_NE(buf, nullptr);
  ASSERT_GT(len, 0u);

  bison_handle decoded = nullptr;
  ASSERT_EQ(bison_deserialize(buf, len, &decoded), BISON_OK);
  ASSERT_NE(decoded, nullptr);
  bison_free_buffer(buf);

  int32_t x = 0;
  float y = 0.f;
  int z = 0;
  char s[16] = {};
  EXPECT_EQ(bison_get_int(decoded, H("x"), &x), BISON_OK);
  EXPECT_EQ(x, 42);
  EXPECT_EQ(bison_get_float(decoded, H("y"), &y), BISON_OK);
  EXPECT_NEAR(y, 2.5f, 1e-4f);
  EXPECT_EQ(bison_get_bool(decoded, H("z"), &z), BISON_OK);
  EXPECT_EQ(z, 1);
  EXPECT_EQ(bison_get_string(decoded, H("s"), s, sizeof s, nullptr), BISON_OK);
  EXPECT_STREQ(s, "hello");
  bison_release(decoded);
}

TEST(SerializationTests, RoundTripsNestedObject) {
  ScopedHandle h{bison_create(0)};
  ScopedHandle child{bison_create(0)};
  ASSERT_EQ(bison_set_string(child, H("city"), "Springfield"), BISON_OK);
  ASSERT_EQ(bison_set_object(h, H("address"), child), BISON_OK);

  uint8_t* buf = nullptr;
  size_t len = 0;
  ASSERT_EQ(bison_serialize(h, &buf, &len), BISON_OK);

  bison_handle decoded = nullptr;
  ASSERT_EQ(bison_deserialize(buf, len, &decoded), BISON_OK);
  bison_free_buffer(buf);

  bison_handle addr = nullptr;
  ASSERT_EQ(bison_get_object(decoded, H("address"), &addr), BISON_OK);
  ASSERT_NE(addr, nullptr);
  char city[32] = {};
  EXPECT_EQ(bison_get_string(addr, H("city"), city, sizeof city, nullptr), BISON_OK);
  EXPECT_STREQ(city, "Springfield");
  bison_release(addr);
  bison_release(decoded);
}

TEST(SerializationTests, RoundTripsEmptyObject) {
  ScopedHandle h{bison_create(0)};
  uint8_t* buf = nullptr;
  size_t len = 0;
  ASSERT_EQ(bison_serialize(h, &buf, &len), BISON_OK);

  bison_handle decoded = nullptr;
  ASSERT_EQ(bison_deserialize(buf, len, &decoded), BISON_OK);
  ASSERT_NE(decoded, nullptr);
  bison_free_buffer(buf);
  bison_release(decoded);
}

TEST(SerializationTests, DeserializeMalformedBufferReturnsParseError) {
  const uint8_t garbage[] = {0xFF, 0x00, 0x01};
  bison_handle decoded = nullptr;
  EXPECT_EQ(bison_deserialize(garbage, sizeof garbage, &decoded), BISON_ERR_PARSE);
  EXPECT_EQ(decoded, nullptr);
}

TEST(SerializationTests, SerializeNullHandleReturnsNullError) {
  uint8_t* buf = nullptr;
  size_t len = 0;
  EXPECT_EQ(bison_serialize(nullptr, &buf, &len), BISON_ERR_NULL);
}

TEST(SerializationTests, DeserializeNullOutReturnsNullError) {
  const uint8_t data[] = {0x00};
  EXPECT_EQ(bison_deserialize(data, sizeof data, nullptr), BISON_ERR_NULL);
}

TEST(SerializationTests, FreeBufferNullIsSafe) {
  bison_free_buffer(nullptr); // must not crash
}

// ═════════════════════════════════════════════════════════════════════════════
// 4. Import helpers
// ═════════════════════════════════════════════════════════════════════════════

TEST(ImportTests, FromJsonObject) {
  ScopedHandle h{bison_from_json(R"({"x": 1, "y": 2})")};
  ASSERT_NE(h.h, nullptr);
  int32_t x = 0, y = 0;
  EXPECT_EQ(bison_get_int(h, H("x"), &x), BISON_OK);
  EXPECT_EQ(bison_get_int(h, H("y"), &y), BISON_OK);
  EXPECT_EQ(x, 1);
  EXPECT_EQ(y, 2);
}

TEST(ImportTests, FromJsonInvalidReturnsNull) {
  bison_handle h = bison_from_json("{broken");
  EXPECT_EQ(h, nullptr);
}

TEST(ImportTests, FromYamlFlatMapping) {
  ScopedHandle h{bison_from_yaml("x: 10\nname: test\n")};
  ASSERT_NE(h.h, nullptr);
  int32_t x = 0;
  EXPECT_EQ(bison_get_int(h, H("x"), &x), BISON_OK);
  EXPECT_EQ(x, 10);
  char buf[32] = {};
  bison_get_string(h, H("name"), buf, sizeof buf, nullptr);
  EXPECT_STREQ(buf, "test");
}

TEST(ImportTests, FromYamlInvalidReturnsNull) {
  bison_handle h = bison_from_yaml("{broken");
  EXPECT_EQ(h, nullptr);
}

// ═════════════════════════════════════════════════════════════════════════════
// 5. Class registry
// ═════════════════════════════════════════════════════════════════════════════

// We clear the class registry using the C++ API to avoid state leakage.
#include "src/bison/bison.hpp"
static void clearClasses() {
  bison_clear_registry();
}

class ClassRegistryTests : public ::testing::Test {
 protected:
  void SetUp() override {
    clearClasses();
  }
  void TearDown() override {
    clearClasses();
  }
};

TEST_F(ClassRegistryTests, AddClassSucceeds) {
  uint32_t key = bison_key("Shape");
  ScopedHandle proto{bison_create(key)};
  bison_set_int(proto, H("sides"), 3);
  EXPECT_EQ(bison_add_class(0, proto, 0, nullptr), BISON_OK);
}

TEST_F(ClassRegistryTests, AddDuplicateClassFails) {
  uint32_t key = bison_key("Widget");
  ScopedHandle p1{bison_create(key)};
  ScopedHandle p2{bison_create(key)};
  EXPECT_EQ(bison_add_class(0, p1, 0, nullptr), BISON_OK);
  EXPECT_EQ(bison_add_class(0, p2, 0, nullptr), BISON_ERR_DUPLICATE);
}

TEST_F(ClassRegistryTests, FindClassReturnsHandle) {
  uint32_t key = bison_key("Gadget");
  ScopedHandle proto{bison_create(key)};
  bison_add_class(0, proto, 0, nullptr);

  bison_handle found = bison_find_class(0, key);
  EXPECT_NE(found, nullptr);
  bison_release(found);
}

TEST_F(ClassRegistryTests, FindClassFromInstantiatedObjectReturnsHandle) {
  uint32_t key = bison_key("WidgetInst");
  ScopedHandle proto{bison_create(key)};
  bison_set_int(proto, H("v"), 1);
  ASSERT_EQ(bison_add_class(0, proto, 0, nullptr), BISON_OK);

  bison_handle found = bison_find_class(0, key);
  EXPECT_NE(found, nullptr);
  bison_release(found);
}

TEST_F(ClassRegistryTests, FindMissingClassReturnsNull) {
  bison_handle found = bison_find_class(0, bison_key("NoSuchClass"));
  EXPECT_EQ(found, nullptr);
}

TEST_F(ClassRegistryTests, AddClassNullHandleReturnsNull) {
  EXPECT_EQ(bison_add_class(0, nullptr, 0, nullptr), BISON_ERR_NULL);
}

// ═════════════════════════════════════════════════════════════════════════════
// 6. Methods
// ═════════════════════════════════════════════════════════════════════════════

static void double_counter_fn(bison_handle self, bison_handle /*params*/, bison_handle result, void* /*user*/) {
  int32_t n = 0;
  bison_get_int(self, H("n"), &n);
  bison_set_int(self, H("n"), n * 2);
  bison_set_int(result, H("value"), n * 2);
}

TEST(MethodTests, AddAndCallMethod) {
  ScopedHandle h{bison_create(0)};
  bison_set_int(h, H("n"), 5);
  EXPECT_EQ(bison_add_method(h, H("double"), double_counter_fn, nullptr, nullptr), BISON_OK);

  ScopedHandle params{bison_create(0)};
  bison_handle result = nullptr;
  EXPECT_EQ(bison_call(h, H("double"), params, &result), BISON_OK);
  ASSERT_NE(result, nullptr);
  int32_t v = 0;
  bison_get_int(result, H("value"), &v);
  EXPECT_EQ(v, 10);
  bison_release(result);
}

TEST(MethodTests, CallMissingMethodReturnsNotFound) {
  ScopedHandle h{bison_create(0)};
  ScopedHandle params{bison_create(0)};
  bison_handle result = nullptr;
  EXPECT_EQ(bison_call(h, H("nonexistent"), params, &result), BISON_ERR_NOT_FOUND);
}

TEST(MethodTests, AddDuplicateMethodFails) {
  ScopedHandle h{bison_create(0)};
  EXPECT_EQ(bison_add_method(h, H("fn"), double_counter_fn, nullptr, nullptr), BISON_OK);
  EXPECT_EQ(bison_add_method(h, H("fn"), double_counter_fn, nullptr, nullptr), BISON_ERR_DUPLICATE);
}

TEST(MethodTests, NullHandleReturnsNullError) {
  ScopedHandle params{bison_create(0)};
  bison_handle result = nullptr;
  EXPECT_EQ(bison_call(nullptr, H("fn"), params, &result), BISON_ERR_NULL);
}

// ═════════════════════════════════════════════════════════════════════════════
// 7. bison_key utility
// ═════════════════════════════════════════════════════════════════════════════

TEST(UtilityTests, BisonKeyHighBitSet) {
  EXPECT_TRUE(bison_key("hello") & 0x80000000u);
}

TEST(UtilityTests, BisonKeySameStringProducesSameHash) {
  EXPECT_EQ(bison_key("foo"), bison_key("foo"));
}

TEST(UtilityTests, BisonKeyDifferentStringsProduceDifferentHashes) {
  EXPECT_NE(bison_key("alpha"), bison_key("beta"));
}

TEST(UtilityTests, BisonKeyNullReturnsZero) {
  EXPECT_EQ(bison_key(nullptr), 0u);
}

// ═════════════════════════════════════════════════════════════════════════════
// 8. Namespace support
// ═════════════════════════════════════════════════════════════════════════════

class CApiNamespaceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    clearClasses();
  }
  void TearDown() override {
    clearClasses();
  }
};

TEST_F(CApiNamespaceTest, AddClassNsSucceeds) {
  bison_hash key = bison_key("table");
  bison_hash ns = bison_key("math");
  ScopedHandle proto{bison_create(key)};
  bison_set_int(proto, bison_key("rows"), 5);
  EXPECT_EQ(bison_add_class(ns, proto, 0, nullptr), BISON_OK);
}

TEST_F(CApiNamespaceTest, SameNameInDifferentNamespacesSucceeds) {
  bison_hash key = bison_key("table");
  ScopedHandle math_proto{bison_create(key)};
  ScopedHandle ikea_proto{bison_create(key)};
  EXPECT_EQ(bison_add_class(bison_key("math"), math_proto, 0, nullptr), BISON_OK);
  EXPECT_EQ(bison_add_class(bison_key("ikea"), ikea_proto, 0, nullptr), BISON_OK);
}

TEST_F(CApiNamespaceTest, DuplicateInSameNamespaceFails) {
  bison_hash key = bison_key("chair");
  bison_hash ns = bison_key("ikea");
  ScopedHandle p1{bison_create(key)};
  ScopedHandle p2{bison_create(key)};
  EXPECT_EQ(bison_add_class(ns, p1, 0, nullptr), BISON_OK);
  EXPECT_EQ(bison_add_class(ns, p2, 0, nullptr), BISON_ERR_DUPLICATE);
}

TEST_F(CApiNamespaceTest, AddClassNsNullHandleReturnsNull) {
  EXPECT_EQ(bison_add_class(bison_key("ns"), nullptr, 0, nullptr), BISON_ERR_NULL);
}

TEST_F(CApiNamespaceTest, InstantiateNsCreatesObjectInNamespace) {
  bison_hash key = bison_key("Vec3");
  bison_hash ns = bison_key("math");
  ScopedHandle proto{bison_create(key)};
  bison_set_int(proto, bison_key("x"), 0);
  ASSERT_EQ(bison_add_class(ns, proto, 0, nullptr), BISON_OK);

  ScopedHandle inst{bison_instantiate(ns, key)};
  ASSERT_NE(inst.h, nullptr);

  // Field inherited from the prototype.
  int32_t v = -1;
  EXPECT_EQ(bison_get_int(inst, bison_key("x"), &v), BISON_OK);
  EXPECT_EQ(v, 0);
}

TEST_F(CApiNamespaceTest, FindClassSearchesCorrectNamespace) {
  bison_hash key = bison_key("Sofa");
  bison_hash ns = bison_key("ikea");
  ScopedHandle proto{bison_create(key)};
  ASSERT_EQ(bison_add_class(ns, proto, 0, nullptr), BISON_OK);

  bison_handle found = bison_find_class(ns, key);
  EXPECT_NE(found, nullptr);
  bison_release(found);
}

// ─────────────────────────────────────────────────────────────────────────────
// PrintTests — bison_print / bison_free_string
// ─────────────────────────────────────────────────────────────────────────────

TEST(PrintTests, DefaultOptionsProducesMultilineOutput) {
  ScopedHandle h{bison_create(0)};
  bison_set_string(h, bison_key("name"), "Alice");
  bison_set_int(h, bison_key("age"), 30);

  char* out = nullptr;
  ASSERT_EQ(bison_print(h, nullptr, &out), BISON_OK);
  ASSERT_NE(out, nullptr);

  const std::string s{out};
  // Default is multiline — must contain newlines and braces.
  EXPECT_NE(s.find('\n'), std::string::npos);
  EXPECT_NE(s.find('{'), std::string::npos);
  EXPECT_NE(s.find('}'), std::string::npos);
  // Values appear in the output.
  EXPECT_NE(s.find("\"Alice\""), std::string::npos);
  EXPECT_NE(s.find("30"), std::string::npos);

  free_and_null(&out);
}

TEST(PrintTests, SingleLineOptionProducesNoNewlines) {
  ScopedHandle h{bison_create(0)};
  bison_set_float(h, bison_key("score"), 7.5f);

  bison_print_options opts{0, nullptr}; // single-line, default indent
  char* out = nullptr;
  ASSERT_EQ(bison_print(h, &opts, &out), BISON_OK);
  ASSERT_NE(out, nullptr);

  const std::string s{out};
  EXPECT_EQ(s.find('\n'), std::string::npos);
  EXPECT_NE(s.find("7.5"), std::string::npos);

  free_and_null(&out);
}

TEST(PrintTests, CustomIndentAppearsInOutput) {
  ScopedHandle h{bison_create(0)};
  bison_set_int(h, bison_key("x"), 1);

  bison_print_options opts{1, "----"};
  char* out = nullptr;
  ASSERT_EQ(bison_print(h, &opts, &out), BISON_OK);
  ASSERT_NE(out, nullptr);

  const std::string s{out};
  EXPECT_NE(s.find("----"), std::string::npos);

  free_and_null(&out);
}

TEST(PrintTests, DisplayNameAttributeUsedAsFieldKey) {
  ScopedHandle h{bison_create(0)};
  bison_attributes meta{};
  meta.display_name = "Player Name";
  ASSERT_EQ(bison_add_field_string(h, bison_key("name"), "Bob", &meta), BISON_OK);

  char* out = nullptr;
  ASSERT_EQ(bison_print(h, nullptr, &out), BISON_OK);
  ASSERT_NE(out, nullptr);

  // DisplayName replaces the hash key in the output.
  EXPECT_NE(std::string{out}.find("Player Name"), std::string::npos);
  EXPECT_NE(std::string{out}.find("\"Bob\""), std::string::npos);

  free_and_null(&out);
}

TEST(PrintTests, MethodsAppearsWithDisplayName) {
  ScopedHandle h{bison_create(0)};
  bison_set_int(h, bison_key("value"), 0);

  bison_attributes m_meta{};
  m_meta.display_name = "Reset";
  m_meta.description = "Resets value to zero";
  ASSERT_EQ(bison_add_method(h, bison_key("reset"), double_counter_fn, nullptr, &m_meta), BISON_OK);
  // Method without attributes.
  ASSERT_EQ(bison_add_method(h, bison_key("ping"), double_counter_fn, nullptr, nullptr), BISON_OK);

  char* out = nullptr;
  ASSERT_EQ(bison_print(h, nullptr, &out), BISON_OK);
  ASSERT_NE(out, nullptr);

  const std::string s{out};
  // Named method key via DisplayName.
  EXPECT_NE(s.find("Reset"), std::string::npos);
  // Attribute summary in method value.
  EXPECT_NE(s.find("displayName"), std::string::npos);
  // Method without attrs uses sentinel.
  EXPECT_NE(s.find("<method>"), std::string::npos);

  free_and_null(&out);
}

TEST(PrintTests, NullHandleReturnsNullError) {
  char* out = nullptr;
  EXPECT_EQ(bison_print(nullptr, nullptr, &out), BISON_ERR_NULL);
  EXPECT_EQ(out, nullptr);
}

TEST(PrintTests, NullOutPointerReturnsNullError) {
  ScopedHandle h{bison_create(0)};
  EXPECT_EQ(bison_print(h, nullptr, nullptr), BISON_ERR_NULL);
}

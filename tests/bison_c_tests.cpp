// MIT License © 2025 Binary Dice Games
// Google Test suite for the pure-C Bison shared-library API.

#include "bison_c.h"
#include "bison_c.hpp"

#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>

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

TEST(SetGetTests, NullHandleReturnsNullError) {
  int32_t v = 0;
  EXPECT_EQ(bison_get_int(nullptr, H("x"), &v), BISON_ERR_NULL);
  EXPECT_EQ(bison_set_int(nullptr, H("x"), 1), BISON_ERR_NULL);
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

TEST(IndexedTests, SizeOfEmptyObjectIsZero) {
  ScopedHandle h{bison_create(0)};
  EXPECT_EQ(bison_size(h), 0u);
}

TEST(IndexedTests, SizeOfNullIsZero) {
  EXPECT_EQ(bison_size(nullptr), 0u);
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
#include "src/core/bison.hpp"
static void clearClasses() {
  bdg::bison::dynamic::getRegistry().wlock()->clear();
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
  EXPECT_EQ(bison_add_class(0, proto, 0), BISON_OK);
}

TEST_F(ClassRegistryTests, AddDuplicateClassFails) {
  uint32_t key = bison_key("Widget");
  ScopedHandle p1{bison_create(key)};
  ScopedHandle p2{bison_create(key)};
  EXPECT_EQ(bison_add_class(0, p1, 0), BISON_OK);
  EXPECT_EQ(bison_add_class(0, p2, 0), BISON_ERR_DUPLICATE);
}

TEST_F(ClassRegistryTests, FindClassReturnsHandle) {
  uint32_t key = bison_key("Gadget");
  ScopedHandle proto{bison_create(key)};
  bison_add_class(0, proto, 0);

  bison_handle found = bison_find_class(0, key);
  EXPECT_NE(found, nullptr);
  bison_release(found);
}

TEST_F(ClassRegistryTests, FindClassFromInstantiatedObjectReturnsHandle) {
  uint32_t key = bison_key("WidgetInst");
  ScopedHandle proto{bison_create(key)};
  bison_set_int(proto, H("v"), 1);
  ASSERT_EQ(bison_add_class(0, proto, 0), BISON_OK);

  bison_handle found = bison_find_class(0, key);
  EXPECT_NE(found, nullptr);
  bison_release(found);
}

TEST_F(ClassRegistryTests, FindMissingClassReturnsNull) {
  bison_handle found = bison_find_class(0, bison_key("NoSuchClass"));
  EXPECT_EQ(found, nullptr);
}

TEST_F(ClassRegistryTests, AddClassNullHandleReturnsNull) {
  EXPECT_EQ(bison_add_class(0, nullptr, 0), BISON_ERR_NULL);
}

// ═════════════════════════════════════════════════════════════════════════════
// 6. Methods
// ═════════════════════════════════════════════════════════════════════════════

static void double_counter_fn(
    bison_handle self,
    bison_handle /*params*/,
    bison_handle result,
    void* /*user*/) {
  int32_t n = 0;
  bison_get_int(self, H("n"), &n);
  bison_set_int(self, H("n"), n * 2);
  bison_set_int(result, H("value"), n * 2);
}

TEST(MethodTests, AddAndCallMethod) {
  ScopedHandle h{bison_create(0)};
  bison_set_int(h, H("n"), 5);
  EXPECT_EQ(
      bison_add_method(h, H("double"), double_counter_fn, nullptr), BISON_OK);

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
  EXPECT_EQ(
      bison_call(h, H("nonexistent"), params, &result), BISON_ERR_NOT_FOUND);
}

TEST(MethodTests, AddDuplicateMethodFails) {
  ScopedHandle h{bison_create(0)};
  EXPECT_EQ(bison_add_method(h, H("fn"), double_counter_fn, nullptr), BISON_OK);
  EXPECT_EQ(
      bison_add_method(h, H("fn"), double_counter_fn, nullptr),
      BISON_ERR_DUPLICATE);
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
  EXPECT_EQ(bison_add_class(ns, proto, 0), BISON_OK);
}

TEST_F(CApiNamespaceTest, SameNameInDifferentNamespacesSucceeds) {
  bison_hash key = bison_key("table");
  ScopedHandle math_proto{bison_create(key)};
  ScopedHandle ikea_proto{bison_create(key)};
  EXPECT_EQ(bison_add_class(bison_key("math"), math_proto, 0), BISON_OK);
  EXPECT_EQ(bison_add_class(bison_key("ikea"), ikea_proto, 0), BISON_OK);
}

TEST_F(CApiNamespaceTest, DuplicateInSameNamespaceFails) {
  bison_hash key = bison_key("chair");
  bison_hash ns = bison_key("ikea");
  ScopedHandle p1{bison_create(key)};
  ScopedHandle p2{bison_create(key)};
  EXPECT_EQ(bison_add_class(ns, p1, 0), BISON_OK);
  EXPECT_EQ(bison_add_class(ns, p2, 0), BISON_ERR_DUPLICATE);
}

TEST_F(CApiNamespaceTest, AddClassNsNullHandleReturnsNull) {
  EXPECT_EQ(bison_add_class(bison_key("ns"), nullptr, 0), BISON_ERR_NULL);
}

TEST_F(CApiNamespaceTest, InstantiateNsCreatesObjectInNamespace) {
  bison_hash key = bison_key("Vec3");
  bison_hash ns = bison_key("math");
  ScopedHandle proto{bison_create(key)};
  bison_set_int(proto, bison_key("x"), 0);
  ASSERT_EQ(bison_add_class(ns, proto, 0), BISON_OK);

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
  ASSERT_EQ(bison_add_class(ns, proto, 0), BISON_OK);

  bison_handle found = bison_find_class(ns, key);
  EXPECT_NE(found, nullptr);
  bison_release(found);
}

// ═════════════════════════════════════════════════════════════════════════════
// 9. C++ RAII wrapper coverage
// ═════════════════════════════════════════════════════════════════════════════

class CxxWrapperTests : public ::testing::Test {
 protected:
  void SetUp() override {
    clearClasses();
  }
  void TearDown() override {
    clearClasses();
  }
};

TEST_F(CxxWrapperTests, SetGetSupportsChainingAndTypedAccess) {
  using bdg::bison::abi::dynamic;

  auto h = dynamic::create();
  h.set(dynamic::key("score"), 42)
      .set(dynamic::key("ratio"), 2.5f)
      .set(dynamic::key("name"), "alice")
      .set(0u, 100)
      .set(1u, 200);

  EXPECT_EQ(h.get<int32_t>(dynamic::key("score")), 42);
  EXPECT_NEAR(h.get<float>(dynamic::key("ratio")), 2.5f, 1e-4f);
  EXPECT_EQ(h.get<std::string>(dynamic::key("name")), "alice");
  EXPECT_EQ(h.get<int32_t>(0u), 100);
  EXPECT_EQ(h.get<int32_t>(1u), 200);
  EXPECT_EQ(h.size(), 2u);
}

TEST_F(CxxWrapperTests, FindClassNamespaceStaticApisWork) {
  using bdg::bison::abi::dynamic;

  bison_hash ns = dynamic::key("math");
  bison_hash klass = dynamic::key("Vec2");

  auto proto = dynamic::create(klass);
  proto.set(dynamic::key("x"), 7);
  dynamic::add_class_ns(ns, proto, 0);

  auto found_ns = dynamic::find_class_ns(ns, klass);
  ASSERT_TRUE(static_cast<bool>(found_ns));
  EXPECT_EQ(found_ns.get<int32_t>(dynamic::key("x")), 7);

  auto found_global = dynamic::find_class(klass);
  EXPECT_FALSE(static_cast<bool>(found_global));
}

TEST_F(CxxWrapperTests, AddMethodWithCapturedLambdaWorksAndPersistsAcrossCopy) {
  using bdg::bison::abi::dynamic;

  auto h = dynamic::create();
  h.set(dynamic::key("n"), 3);

  int calls = 0;
  int factor = 4;
  h.add_method(
      dynamic::key("mul"),
      [&calls, factor](dynamic& self, const dynamic&, dynamic& result) {
        ++calls;
        int32_t n = self.get<int32_t>(dynamic::key("n"));
        result.set(dynamic::key("value"), n * factor);
      });

  // Copy should keep method callback state alive.
  dynamic h2 = h;
  auto params = dynamic::create();
  auto result = h2.call(dynamic::key("mul"), params);

  EXPECT_EQ(result.get<int32_t>(dynamic::key("value")), 12);
  EXPECT_EQ(calls, 1);
}

TEST_F(CxxWrapperTests, MissingMethodThrowsRuntimeError) {
  using bdg::bison::abi::dynamic;

  auto h = dynamic::create();
  auto params = dynamic::create();
  EXPECT_THROW(
      (void)h.call(dynamic::key("does_not_exist"), params), std::runtime_error);
}

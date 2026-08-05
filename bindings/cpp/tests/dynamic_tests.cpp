// MIT License © 2025 Binary Dice Games
// Google Test suite for the header-only C++ ABI binding's dynamic-object
// API (bindings/cpp/include/bison/dynamic.hpp), wrapping bison_c.h /
// bison_abi.

#include "bison/dynamic.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

using namespace bdg::bison::abi;

// bdg::bison::abi::key_t must stay explicitly qualified below: glibc's
// <sys/types.h> (pulled in transitively by <gtest/gtest.h>) also defines a
// global `key_t` typedef, so a bare `key_t` after `using namespace
// bdg::bison::abi;` is ambiguous (same pitfall documented in
// tests/bison_c_tests.cpp for the internal C++ API).
using bison_key_t = bdg::bison::abi::key_t;

// ═════════════════════════════════════════════════════════════════════════════
// 1. Hashing / keys
// ═════════════════════════════════════════════════════════════════════════════

TEST(Hashing, SameStringHashesStably) {
  EXPECT_EQ(hash("velocity"), hash("velocity"));
}

TEST(Hashing, DifferentStringsHashDifferently) {
  EXPECT_NE(hash("velocity"), hash("score"));
}

TEST(Hashing, MsbIsAlwaysSet) {
  EXPECT_NE(hash("anything") & 0x80000000u, 0u);
}

TEST(Hashing, LiteralMatchesRuntimeHash) {
  EXPECT_EQ(static_cast<hash_t>("score"_key), hash("score"));
}

TEST(Hashing, LiteralIsConstexpr) {
  constexpr hash_t h = "score"_key;
  static_assert(h == hash("score"));
  EXPECT_EQ(h, hash("score"));
}

// ═════════════════════════════════════════════════════════════════════════════
// 2. Lifecycle
// ═════════════════════════════════════════════════════════════════════════════

TEST(Lifecycle, DefaultConstructIsValid) {
  dynamic obj;
  EXPECT_TRUE(obj.valid());
}

TEST(Lifecycle, MoveInvalidatesSource) {
  dynamic obj;
  dynamic moved = std::move(obj);
  EXPECT_FALSE(obj.valid());
  EXPECT_TRUE(moved.valid());
}

TEST(Lifecycle, CloneIsIndependent) {
  dynamic obj;
  obj["n"_key] = int32_t{1};
  dynamic clone = obj.clone();
  clone["n"_key] = int32_t{2};
  EXPECT_EQ(obj["n"_key].as<int32_t>(), 1);
  EXPECT_EQ(clone["n"_key].as<int32_t>(), 2);
}

TEST(Lifecycle, CopyConstructorClones) {
  dynamic obj;
  obj["n"_key] = int32_t{1};
  dynamic copy(obj);
  copy["n"_key] = int32_t{2};
  EXPECT_EQ(obj["n"_key].as<int32_t>(), 1);
  EXPECT_EQ(copy["n"_key].as<int32_t>(), 2);
}

TEST(Lifecycle, AddRefSharesMutations) {
  dynamic obj;
  obj["n"_key] = int32_t{10};
  dynamic ref = obj.add_ref();
  obj["n"_key] = int32_t{20};
  EXPECT_EQ(ref["n"_key].as<int32_t>(), 20);
}

TEST(Lifecycle, BorrowDoesNotOwn) {
  bison_handle raw = bison_create(0);
  {
    dynamic view = dynamic::borrow(raw);
    view["n"_key] = int32_t{1};
  }
  // `raw` must still be valid -- the borrowed view never released it.
  int32_t v = 0;
  EXPECT_EQ(bison_get_int(raw, bison_key("n"), &v), BISON_OK);
  EXPECT_EQ(v, 1);
  bison_release(raw);
}

// ═════════════════════════════════════════════════════════════════════════════
// 3. Field access
// ═════════════════════════════════════════════════════════════════════════════

TEST(FieldAccess, ScalarRoundTrip) {
  dynamic obj;
  obj["score"_key] = int32_t{42};
  obj["speed"_key] = 9.5f;
  obj["alive"_key] = true;
  obj["name"_key] = std::string{"hero"};

  EXPECT_EQ(obj["score"_key].as<int32_t>(), 42);
  EXPECT_FLOAT_EQ(obj["speed"_key].as<float>(), 9.5f);
  EXPECT_TRUE(obj["alive"_key].as<bool>());
  EXPECT_EQ(obj["name"_key].as<std::string>(), "hero");
}

TEST(FieldAccess, ImplicitConversionReadsField) {
  dynamic obj;
  obj["score"_key] = int32_t{7};
  int32_t score = obj["score"_key];
  EXPECT_EQ(score, 7);
}

TEST(FieldAccess, TypeMismatchThrows) {
  dynamic obj;
  obj["score"_key] = int32_t{1};
  EXPECT_THROW(obj["score"_key] = 1.5f, bison_exception);
}

TEST(FieldAccess, TypeMismatchReadThrowsWithCorrectCode) {
  dynamic obj;
  obj["score"_key] = int32_t{1};
  try {
    obj["score"_key].as<float>();
    FAIL() << "expected bison_exception";
  } catch (const bison_exception& e) {
    EXPECT_EQ(e.code, BISON_ERR_TYPE);
  }
}

TEST(FieldAccess, NestedObject) {
  dynamic obj;
  dynamic child;
  child["city"_key] = std::string{"Springfield"};
  obj["address"_key] = child;

  dynamic addr = obj["address"_key];
  EXPECT_EQ(addr["city"_key].as<std::string>(), "Springfield");
}

TEST(FieldAccess, NullNestedObjectReturnsNulloptFromAsObject) {
  dynamic obj;
  obj["ref"_key] = nullptr;
  EXPECT_FALSE(obj["ref"_key].as_object().has_value());
}

TEST(FieldAccess, IndexedFieldsAndSize) {
  dynamic obj;
  obj[0] = std::string{"red"};
  obj[1] = std::string{"green"};
  obj[2] = std::string{"blue"};
  EXPECT_EQ(obj.size(), 3u);
  EXPECT_EQ(obj[1].as<std::string>(), "green");
}

TEST(FieldAccess, IndexedTypeLock) {
  dynamic obj;
  obj[0] = int32_t{10};
  EXPECT_THROW(obj[0] = 1.5f, bison_exception);
}

TEST(FieldAccess, IndexedBoolRoundTrip) {
  dynamic obj;
  obj[0] = true;
  obj[1] = false;
  EXPECT_TRUE(obj[0].as<bool>());
  EXPECT_FALSE(obj[1].as<bool>());
}

TEST(FieldAccess, IndexedKeyRoundTrip) {
  dynamic obj;
  obj[0] = bison_key_t{"hero"};
  EXPECT_EQ(obj[0].as<bison_key_t>().id, hash("hero"));
}

TEST(FieldAccess, IndexedObjectRoundTrip) {
  dynamic obj;
  dynamic child;
  child["v"_key] = int32_t{9};
  obj[0] = child;
  dynamic out = obj[0];
  EXPECT_EQ(out["v"_key].as<int32_t>(), 9);
}

TEST(FieldAccess, IndexedNullObjectRoundTrip) {
  dynamic obj;
  obj[0] = nullptr;
  EXPECT_FALSE(obj[0].as_object().has_value());
}

TEST(FieldAccess, DynamicAsHelper) {
  dynamic obj;
  obj["score"_key] = int32_t{5};
  EXPECT_EQ(obj.as<int32_t>("score"_key), 5);
}

// ═════════════════════════════════════════════════════════════════════════════
// key_t-typed field access
// ═════════════════════════════════════════════════════════════════════════════

TEST(KeyTypedFieldAccess, SetKeyRoundTripsWithAlreadyHashedValue) {
  dynamic obj;
  obj["id"_key] = bison_key_t{"hero"};
  EXPECT_EQ(obj["id"_key].as<bison_key_t>().id, hash("hero"));
}

TEST(KeyTypedFieldAccess, DistinctFromInt32Field) {
  dynamic obj;
  obj["plain_int"_key] = int32_t{42};
  EXPECT_THROW(obj["plain_int"_key] = bison_key_t{"anything"}, bison_exception);
}

TEST(KeyTypedFieldAccess, AddFieldKeyDeclaresAndRejectsDuplicate) {
  dynamic obj;
  EXPECT_TRUE(obj.addFieldKey("id"_key, bison_key_t{"hero"}));
  EXPECT_EQ(obj["id"_key].as<bison_key_t>().id, hash("hero"));
  EXPECT_FALSE(obj.addFieldKey("id"_key, bison_key_t{"other"}));
}

// ═════════════════════════════════════════════════════════════════════════════
// Methods
// ═════════════════════════════════════════════════════════════════════════════

TEST(Methods, AddMethodAndCall) {
  dynamic calc;
  calc.addMethod("add"_key, [](dynamic&, const dynamic& params, dynamic& result) {
    result["value"_key] = params["a"_key].as<int32_t>() + params["b"_key].as<int32_t>();
  });

  dynamic args;
  args["a"_key] = int32_t{10};
  args["b"_key] = int32_t{32};
  dynamic out = calc.call("add"_key, args);
  EXPECT_EQ(out["value"_key].as<int32_t>(), 42);
}

TEST(Methods, MethodMutatesSelf) {
  dynamic calc;
  calc["total"_key] = int32_t{0};
  calc.addMethod("accumulate"_key, [](dynamic& self, const dynamic& params, dynamic& result) {
    int32_t total = self["total"_key].as<int32_t>() + params["n"_key].as<int32_t>();
    self["total"_key] = total;
    result["total"_key] = total;
  });

  for (int32_t i = 1; i <= 3; ++i) {
    dynamic p;
    p["n"_key] = i;
    calc.call("accumulate"_key, p);
  }
  EXPECT_EQ(calc["total"_key].as<int32_t>(), 6);
}

TEST(Methods, CallUnknownMethodThrowsNotFound) {
  dynamic obj;
  try {
    obj.call("nope"_key);
    FAIL() << "expected bison_exception";
  } catch (const bison_exception& e) {
    EXPECT_EQ(e.code, BISON_ERR_NOT_FOUND);
  }
}

// ═════════════════════════════════════════════════════════════════════════════
// Class registry / inheritance
// ═════════════════════════════════════════════════════════════════════════════

class ClassRegistryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dynamic::clear_registry();
  }
  void TearDown() override {
    dynamic::clear_registry();
  }
};

TEST_F(ClassRegistryTest, Inheritance) {
  dynamic shape{"Shape"_key};
  shape["color"_key] = std::string{"black"};
  ASSERT_TRUE(dynamic::addClass(shape));

  dynamic circle{"Circle"_key};
  circle["radius"_key] = 1.0f;
  ASSERT_TRUE(dynamic::addClass(circle, "Shape"_key));

  dynamic c = dynamic::instantiate("Circle"_key);
  EXPECT_EQ(c["color"_key].as<std::string>(), "black"); // inherited default
  EXPECT_FLOAT_EQ(c["radius"_key].as<float>(), 1.0f);
}

TEST_F(ClassRegistryTest, DuplicateClassReturnsFalse) {
  dynamic proto{"Shape"_key};
  ASSERT_TRUE(dynamic::addClass(proto));
  dynamic dup{"Shape"_key};
  EXPECT_FALSE(dynamic::addClass(dup));
}

TEST_F(ClassRegistryTest, FindClass) {
  dynamic proto{"Shape"_key};
  ASSERT_TRUE(dynamic::addClass(proto));
  EXPECT_TRUE(dynamic::find_class("Shape"_key).has_value());
  EXPECT_FALSE(dynamic::find_class("DoesNotExist"_key).has_value());
}

TEST_F(ClassRegistryTest, NamespacesIsolateSameName) {
  dynamic math_table{"table"_key};
  math_table["rows"_key] = int32_t{1};
  ASSERT_TRUE(dynamic::addClass("math"_key, math_table));

  dynamic ikea_table{"table"_key};
  ikea_table["legs"_key] = int32_t{4};
  ASSERT_TRUE(dynamic::addClass("ikea"_key, ikea_table));

  dynamic mt = dynamic::instantiate("math"_key, "table"_key);
  dynamic it = dynamic::instantiate("ikea"_key, "table"_key);
  EXPECT_EQ(mt["rows"_key].as<int32_t>(), 1);
  EXPECT_EQ(it["legs"_key].as<int32_t>(), 4);
}

TEST_F(ClassRegistryTest, ClassAndFieldAttributes) {
  dynamic proto{"Widget"_key};
  attributes field_meta;
  field_meta.description = "a counter";
  field_meta.required = true;
  ASSERT_TRUE(proto.addField("count"_key, int32_t{0}, field_meta));

  attributes class_meta;
  class_meta.display_name = "Widget class";
  ASSERT_TRUE(dynamic::addClass(proto, bison_key_t{0U}, class_meta));

  attributes read_back = dynamic::class_attributes("Widget"_key);
  ASSERT_TRUE(read_back.display_name.has_value());
  EXPECT_EQ(*read_back.display_name, "Widget class");

  dynamic w = dynamic::instantiate("Widget"_key);
  attributes fattrs = w.field_attributes("count"_key);
  ASSERT_TRUE(fattrs.description.has_value());
  EXPECT_EQ(*fattrs.description, "a counter");
  EXPECT_TRUE(fattrs.required);
}

TEST_F(ClassRegistryTest, AddFieldRejectsDuplicate) {
  dynamic obj;
  EXPECT_TRUE(obj.addField("x"_key, int32_t{1}));
  EXPECT_FALSE(obj.addField("x"_key, int32_t{2}));
}

TEST_F(ClassRegistryTest, RegisteredMethodSurvivesPrototypeDestruction) {
  // A class's methods must keep working after the registering prototype
  // wrapper is destroyed -- regression coverage for the method-callback
  // trampoline lifetime (see dynamic.hpp's detail::method_registry doc
  // comment).
  {
    dynamic proto{"Doubler"_key};
    proto.addMethod("double"_key, [](dynamic&, const dynamic& params, dynamic& result) {
      result["value"_key] = params["n"_key].as<int32_t>() * 2;
    });
    ASSERT_TRUE(dynamic::addClass(proto));
  } // `proto` destroyed here.

  dynamic inst = dynamic::instantiate("Doubler"_key);
  dynamic args;
  args["n"_key] = int32_t{21};
  dynamic out = inst.call("double"_key, args);
  EXPECT_EQ(out["value"_key].as<int32_t>(), 42);
}

// ═════════════════════════════════════════════════════════════════════════════
// Vector fields — registration (addField) and direct read/write
// (operator[](key_t)'s operator=/as<T>())
// ═════════════════════════════════════════════════════════════════════════════

TEST(VectorFields, RegistrationSucceedsAndRejectsDuplicate) {
  dynamic obj;
  EXPECT_TRUE(obj.addField("flags"_key, std::vector<bool>{true, false, true}));
  EXPECT_FALSE(obj.addField("flags"_key, std::vector<bool>{false}));
  EXPECT_TRUE(obj.addField("ints"_key, std::vector<int32_t>{1, 2, 3}));
  EXPECT_TRUE(obj.addField("floats"_key, std::vector<float>{1.5f, 2.5f}));
  EXPECT_TRUE(obj.addField("bytes"_key, std::vector<uint8_t>{0x01, 0x02}));
}

TEST(VectorFields, RegisteredFieldIsReadable) {
  dynamic obj;
  ASSERT_TRUE(obj.addField("ints"_key, std::vector<int32_t>{1, 2, 3}));
  EXPECT_EQ(obj["ints"_key].as<std::vector<int32_t>>(), (std::vector<int32_t>{1, 2, 3}));
}

TEST(VectorFields, IntRoundTrip) {
  dynamic obj;
  obj["ints"_key] = std::vector<int32_t>{1, 2, 3};
  EXPECT_EQ(obj["ints"_key].as<std::vector<int32_t>>(), (std::vector<int32_t>{1, 2, 3}));
}

TEST(VectorFields, BoolRoundTrip) {
  dynamic obj;
  obj["flags"_key] = std::vector<bool>{true, false, true};
  EXPECT_EQ(obj["flags"_key].as<std::vector<bool>>(), (std::vector<bool>{true, false, true}));
}

TEST(VectorFields, FloatRoundTrip) {
  dynamic obj;
  obj["ratios"_key] = std::vector<float>{1.5f, 2.5f};
  auto v = obj["ratios"_key].as<std::vector<float>>();
  ASSERT_EQ(v.size(), 2u);
  EXPECT_FLOAT_EQ(v[0], 1.5f);
  EXPECT_FLOAT_EQ(v[1], 2.5f);
}

TEST(VectorFields, BytesRoundTrip) {
  dynamic obj;
  obj["blob"_key] = std::vector<uint8_t>{0, 1, 255};
  EXPECT_EQ(obj["blob"_key].as<std::vector<uint8_t>>(), (std::vector<uint8_t>{0, 1, 255}));
}

TEST(VectorFields, AssignmentReplacesExistingContents) {
  dynamic obj;
  obj["ints"_key] = std::vector<int32_t>{1, 2, 3};
  obj["ints"_key] = std::vector<int32_t>{9, 9};
  EXPECT_EQ(obj["ints"_key].as<std::vector<int32_t>>(), (std::vector<int32_t>{9, 9}));
}

TEST(VectorFields, WrongTypeReadThrows) {
  dynamic obj;
  obj["x"_key] = int32_t{1};
  EXPECT_THROW(obj["x"_key].as<std::vector<int32_t>>(), bison_exception);
}

TEST(VectorFields, IndexedAssignmentThrows) {
  dynamic obj;
  EXPECT_THROW(obj[0] = std::vector<int32_t>{1}, std::logic_error);
}

TEST(VectorFields, IndexedReadThrows) {
  dynamic obj;
  obj[0] = int32_t{1};
  EXPECT_THROW(obj[0].as<std::vector<int32_t>>(), std::logic_error);
}

// ═════════════════════════════════════════════════════════════════════════════
// Serialization
// ═════════════════════════════════════════════════════════════════════════════

TEST(Serialization, FromJsonRoundTrip) {
  dynamic obj = dynamic::from_json(R"({"x": 1, "y": 2.5})");
  EXPECT_EQ(obj["x"_key].as<int32_t>(), 1);
  EXPECT_FLOAT_EQ(obj["y"_key].as<float>(), 2.5f);
}

TEST(Serialization, FromYamlRoundTrip) {
  dynamic obj = dynamic::from_yaml("x: 10\nname: test\n");
  EXPECT_EQ(obj["x"_key].as<int32_t>(), 10);
  EXPECT_EQ(obj["name"_key].as<std::string>(), "test");
}

TEST(Serialization, InvalidJsonThrows) {
  EXPECT_THROW(dynamic::from_json("not json"), bison_exception);
}

TEST(Serialization, ToJsonProducesNonEmptyString) {
  dynamic obj = dynamic::from_json(R"({"x": 1})");
  std::string json = obj.to_json(-1);
  EXPECT_FALSE(json.empty());
  EXPECT_NE(json.find('1'), std::string::npos);
}

TEST(Serialization, PrettyProducesNonEmptyString) {
  dynamic obj;
  obj["x"_key] = int32_t{1};
  EXPECT_FALSE(obj.pretty().empty());
}

// ═════════════════════════════════════════════════════════════════════════════
// Binary serialization (serialize() / deserialize())
// ═════════════════════════════════════════════════════════════════════════════

TEST(BinarySerialization, RoundTripsScalarFields) {
  dynamic obj;
  obj["x"_key] = int32_t{42};
  obj["y"_key] = 2.5f;
  obj["s"_key] = std::string{"hello"};

  std::vector<uint8_t> buf = obj.serialize();
  EXPECT_FALSE(buf.empty());

  dynamic decoded = dynamic::deserialize(buf);
  EXPECT_EQ(decoded["x"_key].as<int32_t>(), 42);
  EXPECT_FLOAT_EQ(decoded["y"_key].as<float>(), 2.5f);
  EXPECT_EQ(decoded["s"_key].as<std::string>(), "hello");
}

TEST(BinarySerialization, RoundTripsNestedObject) {
  dynamic obj;
  dynamic child;
  child["city"_key] = std::string{"Springfield"};
  obj["address"_key] = child;

  dynamic decoded = dynamic::deserialize(obj.serialize());
  dynamic addr = decoded["address"_key];
  EXPECT_EQ(addr["city"_key].as<std::string>(), "Springfield");
}

TEST(BinarySerialization, RoundTripsThroughRawPointerOverload) {
  dynamic obj;
  obj["x"_key] = int32_t{7};
  std::vector<uint8_t> buf = obj.serialize();

  dynamic decoded = dynamic::deserialize(buf.data(), buf.size());
  EXPECT_EQ(decoded["x"_key].as<int32_t>(), 7);
}

TEST(BinarySerialization, MalformedBufferThrowsParseError) {
  std::vector<uint8_t> garbage{0xFF, 0x00, 0x01};
  try {
    dynamic::deserialize(garbage);
    FAIL() << "expected bison_exception";
  } catch (const bison_exception& e) {
    EXPECT_EQ(e.code, BISON_ERR_PARSE);
  }
}

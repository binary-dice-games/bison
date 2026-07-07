// MIT License © 2025 Binary Dice Games
// Comprehensive unit tests for the Bison library using Google Test.

#include "src/bison/bison.hpp"

#include <gtest/gtest.h>
#include <array>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <vector>

// Use the full namespace to avoid ambiguity with POSIX names
// (e.g. POSIX defines key_t as int).
using namespace bdg::bison;
// Bring bison's key_t into the local scope unambiguously.
using bison_key_t = bdg::bison::key_t;

// ─────────────────────────────────────────────────────────────────────────────
// Helper: reset the global class registry between test suites to avoid
// state leaking from one class-registration test into another.
// ─────────────────────────────────────────────────────────────────────────────
static void clearClassRegistry() {
  dynamic::getRegistry().wlock()->clear();
}

// ═════════════════════════════════════════════════════════════════════════════
// 1. Hashing and key_t
// ═════════════════════════════════════════════════════════════════════════════

TEST(HashTests, FNV1aHighBitAlwaysSet) {
  // The hash function always sets the high bit so named keys are
  // distinguishable from small numeric indices.
  EXPECT_TRUE(hash("hello") & 0x80000000u);
  EXPECT_TRUE(hash("world") & 0x80000000u);
  EXPECT_TRUE(hash("") & 0x80000000u);
}

TEST(HashTests, SameStringProducesSameHash) {
  EXPECT_EQ(hash("foo"), hash("foo"));
  EXPECT_EQ("bison"_key, hash("bison"));
}

TEST(HashTests, DifferentStringsProduceDifferentHashes) {
  EXPECT_NE(hash("alpha"), hash("beta"));
}

TEST(HashTests, KeyLiteralMatchesRuntimeHash) {
  hash_t compile_time = "velocity"_key;
  hash_t runtime = hash("velocity");
  EXPECT_EQ(compile_time, runtime);
}

TEST(HashTests, KeyTConstructors) {
  bison_key_t from_literal = "name"_key;
  bison_key_t from_cstr = bison_key_t{"name"};
  bison_key_t from_string = bison_key_t{std::string{"name"}};
  EXPECT_EQ(from_literal.id, from_cstr.id);
  EXPECT_EQ(from_literal.id, from_string.id);
}

TEST(HashTests, KeyTImplicitConversionToHashT) {
  bison_key_t k = "score"_key;
  hash_t h = k;
  EXPECT_EQ(h, hash("score"));
}

// ═════════════════════════════════════════════════════════════════════════════
// 2. byte_swap
// ═════════════════════════════════════════════════════════════════════════════

TEST(ByteSwapTests, Roundtrip) {
  int32_t original = 0x01020304;
  int32_t swapped = byte_swap(byte_swap(original));
  EXPECT_EQ(original, swapped);
}

TEST(ByteSwapTests, FloatRoundtrip) {
  float original = 3.14159f;
  float restored = byte_swap(byte_swap(original));
  EXPECT_FLOAT_EQ(original, restored);
}

// ═════════════════════════════════════════════════════════════════════════════
// 3. stream_serializer / stream_deserializer (low-level stream round-trips)
// ═════════════════════════════════════════════════════════════════════════════

TEST(StreamSerializerTests, Int32RoundTrip) {
  std::stringstream ss;
  stream_serializer out(ss);
  out.write(int32_t{12345});

  stream_deserializer in(ss);
  EXPECT_EQ(in.read<int32_t>(), 12345);
}

TEST(StreamSerializerTests, FloatRoundTrip) {
  std::stringstream ss;
  {
    stream_serializer s{ss};
    s.write(float{2.71828f});
  }
  stream_deserializer d{ss};
  EXPECT_FLOAT_EQ(d.read<float>(), 2.71828f);
}

TEST(StreamSerializerTests, BoolRoundTrip) {
  std::stringstream ss;
  {
    stream_serializer s{ss};
    s.write(true);
  }
  stream_deserializer d{ss};
  EXPECT_TRUE(d.read<bool>());
}

TEST(StreamSerializerTests, StringRoundTrip) {
  std::stringstream ss;
  {
    stream_serializer s{ss};
    s.write(std::string{"hello bison"});
  }

  std::string result;
  stream_deserializer d{ss};
  d.read(result);
  EXPECT_EQ(result, "hello bison");
}

TEST(StreamSerializerTests, EmptyStringRoundTrip) {
  std::stringstream ss;
  {
    stream_serializer s{ss};
    s.write(std::string{""});
  }
  std::string result{"non-empty"};
  stream_deserializer d{ss};
  d.read(result);
  EXPECT_EQ(result, "");
}

TEST(StreamSerializerTests, VectorInt32RoundTrip) {
  std::stringstream ss;
  std::vector<int32_t> v{1, 2, 3, 4, 5};
  {
    stream_serializer s{ss};
    s.write(v);
  }

  std::vector<int32_t> out;
  stream_deserializer d{ss};
  d.read(out);
  EXPECT_EQ(out, v);
}

TEST(StreamSerializerTests, VectorFloatRoundTrip) {
  std::stringstream ss;
  std::vector<float> v{1.1f, 2.2f, 3.3f};
  {
    stream_serializer s{ss};
    s.write(v);
  }

  std::vector<float> out;
  stream_deserializer d{ss};
  d.read(out);
  ASSERT_EQ(out.size(), v.size());
  for (size_t i = 0; i < v.size(); ++i)
    EXPECT_FLOAT_EQ(out[i], v[i]);
}

TEST(StreamSerializerTests, SpanInt32RoundTripToVector) {
  std::stringstream ss;
  std::array<int32_t, 4> values{11, 22, 33, 44};
  {
    stream_serializer s{ss};
    s.write(std::span<const int32_t>(values));
  }

  std::vector<int32_t> out;
  stream_deserializer d{ss};
  d.read(out);
  ASSERT_EQ(out.size(), values.size());
  for (size_t i = 0; i < values.size(); ++i)
    EXPECT_EQ(out[i], values[i]);
}

TEST(StreamSerializerTests, SpanFloatRoundTripIntoFixedBuffer) {
  std::stringstream ss;
  std::array<float, 3> values{1.25f, 2.5f, 5.0f};
  {
    stream_serializer s{ss};
    s.write(std::span<const float>(values));
  }

  std::array<float, 3> out{};
  stream_deserializer d{ss};
  d.read(std::span<float>(out));
  for (size_t i = 0; i < values.size(); ++i)
    EXPECT_FLOAT_EQ(out[i], values[i]);
}

TEST(StreamSerializerTests, SpanReadSizeMismatchThrows) {
  std::stringstream ss;
  std::array<int32_t, 3> values{1, 2, 3};
  {
    stream_serializer s{ss};
    s.write(std::span<const int32_t>(values));
  }

  std::array<int32_t, 2> too_small{};
  stream_deserializer d{ss};
  EXPECT_THROW(d.read(std::span<int32_t>(too_small)), std::runtime_error);
}

TEST(StreamSerializerTests, StringViewWriteRoundTrip) {
  std::stringstream ss;
  std::string backing = "hello view";
  std::string_view view{backing};
  {
    stream_serializer s{ss};
    s.write(view);
  }

  std::string out;
  stream_deserializer d{ss};
  d.read(out);
  EXPECT_EQ(out, backing);
}

TEST(StreamSerializerTests, StringViewReadAliasesStorage) {
  std::stringstream ss;
  {
    stream_serializer s{ss};
    s.write(std::string{"alias me"});
  }

  std::string storage;
  std::string_view view;
  stream_deserializer d{ss};
  d.read(view, storage);
  EXPECT_EQ(view, "alias me");
  EXPECT_EQ(view.data(), storage.data());
}

TEST(StreamSerializerTests, MultipleValuesInOrder) {
  std::stringstream ss;
  stream_serializer s{ss};
  s.write(int32_t{10}).write(int32_t{20}).write(int32_t{30});

  stream_deserializer d{ss};
  EXPECT_EQ(d.read<int32_t>(), 10);
  EXPECT_EQ(d.read<int32_t>(), 20);
  EXPECT_EQ(d.read<int32_t>(), 30);
}

// ═════════════════════════════════════════════════════════════════════════════
// 3b. buffer_serializer / buffer_deserializer (in-memory round-trips)
// ═════════════════════════════════════════════════════════════════════════════

TEST(BufferSerializerTests, Int32RoundTrip) {
  buffer_serializer out;
  out.write(int32_t{12345});
  auto buf = out.release();

  buffer_deserializer in(buf);
  EXPECT_EQ(in.read<int32_t>(), 12345);
}

TEST(BufferSerializerTests, FloatRoundTrip) {
  buffer_serializer out;
  out.write(float{2.71828f});
  auto buf = out.release();

  buffer_deserializer in(buf);
  EXPECT_FLOAT_EQ(in.read<float>(), 2.71828f);
}

TEST(BufferSerializerTests, BoolRoundTrip) {
  buffer_serializer out;
  out.write(true);
  auto buf = out.release();

  buffer_deserializer in(buf);
  EXPECT_TRUE(in.read<bool>());
}

TEST(BufferSerializerTests, StringRoundTrip) {
  buffer_serializer out;
  out.write(std::string{"hello buffer"});
  auto buf = out.release();

  buffer_deserializer in(buf);
  std::string result;
  in.read(result);
  EXPECT_EQ(result, "hello buffer");
}

TEST(BufferSerializerTests, VectorInt32RoundTrip) {
  std::vector<int32_t> v{10, 20, 30, 40, 50};
  buffer_serializer out;
  out.write(v);
  auto buf = out.release();

  buffer_deserializer in(buf);
  std::vector<int32_t> result;
  in.read(result);
  EXPECT_EQ(result, v);
}

TEST(BufferSerializerTests, VectorFloatRoundTrip) {
  std::vector<float> v{1.1f, 2.2f, 3.3f};
  buffer_serializer out;
  out.write(v);
  auto buf = out.release();

  buffer_deserializer in(buf);
  std::vector<float> result;
  in.read(result);
  ASSERT_EQ(result.size(), v.size());
  for (size_t i = 0; i < v.size(); ++i)
    EXPECT_FLOAT_EQ(result[i], v[i]);
}

TEST(BufferSerializerTests, MultipleValuesInOrder) {
  buffer_serializer out;
  out.write(int32_t{10}).write(int32_t{20}).write(int32_t{30});
  auto buf = out.release();

  buffer_deserializer in(buf);
  EXPECT_EQ(in.read<int32_t>(), 10);
  EXPECT_EQ(in.read<int32_t>(), 20);
  EXPECT_EQ(in.read<int32_t>(), 30);
}

TEST(BufferSerializerTests, UnderflowThrows) {
  buffer_serializer out;
  out.write(int32_t{1});
  auto buf = out.release();

  buffer_deserializer in(buf);
  in.read<int32_t>(); // consume the one value
  EXPECT_THROW(in.read<int32_t>(), std::runtime_error);
}

TEST(BufferSerializerTests, BufferEquivalentToStream) {
  // Verify that buffer_serializer produces the same bytes as stream_serializer.
  std::stringstream ss;
  {
    stream_serializer s{ss};
    s.write(int32_t{42});
    s.write(float{3.14f});
    s.write(std::string{"bison"});
  }
  std::string stream_bytes = ss.str();

  buffer_serializer buf_out;
  buf_out.write(int32_t{42});
  buf_out.write(float{3.14f});
  buf_out.write(std::string{"bison"});
  auto buf_bytes = buf_out.release();

  ASSERT_EQ(stream_bytes.size(), buf_bytes.size());
  EXPECT_EQ(0, std::memcmp(stream_bytes.data(), buf_bytes.data(), stream_bytes.size()));
}

TEST(BufferSerializerTests, DynamicObjectRoundTrip) {
  dynamic obj{"BufTest"_key};
  obj["x"_key] = int32_t{7};
  obj["label"_key] = std::string{"hello"};

  buffer_serializer out;
  obj.serialize(out);
  auto buf = out.release();

  buffer_deserializer in(buf);
  auto copy = dynamic::deserialize(in);

  EXPECT_EQ(copy["x"_key].as<int32_t>(), 7);
  EXPECT_EQ(copy["label"_key].as<std::string>(), "hello");
}

TEST(BufferSerializerTests, StringViewRoundTrip) {
  std::string backing = "view data";
  buffer_serializer out;
  out.write(std::string_view{backing});
  auto buf = out.release();

  buffer_deserializer in(buf);
  std::string result;
  in.read(result);
  EXPECT_EQ(result, backing);
}

// ═════════════════════════════════════════════════════════════════════════════
// 4. field construction and type system
// ═════════════════════════════════════════════════════════════════════════════

TEST(FieldTests, DefaultConstructionIsMonostate) {
  field f;
  EXPECT_TRUE(f.is<std::monostate>());
}

TEST(FieldTests, ConstructBool) {
  field f{true};
  EXPECT_TRUE(f.is<bool>());
  EXPECT_TRUE(f.as<bool>());
}

TEST(FieldTests, ConstructInt32) {
  field f{int32_t{42}};
  EXPECT_TRUE(f.is<int32_t>());
  EXPECT_EQ(f.as<int32_t>(), 42);
}

TEST(FieldTests, ConstructFloat) {
  field f{3.14f};
  EXPECT_TRUE(f.is<float>());
  EXPECT_FLOAT_EQ(f.as<float>(), 3.14f);
}

TEST(FieldTests, ConstructString) {
  field f{std::string{"hello"}};
  EXPECT_TRUE(f.is<std::string>());
  EXPECT_EQ(f.as<std::string>(), "hello");
}

TEST(FieldTests, ImplicitConversionToConstStringRefAvoidsCopy) {
  const field f{std::string{"hello"}};
  const std::string& by_ref = static_cast<const std::string&>(f);
  const std::string& stored = f.as<std::string>();
  EXPECT_EQ(by_ref, "hello");
  EXPECT_EQ(&by_ref, &stored);
}

TEST(FieldTests, ConstructFromStringLiteralPromotesToString) {
  // const char* should be promoted to std::string automatically.
  field f{"world"};
  EXPECT_TRUE(f.is<std::string>());
  EXPECT_EQ(f.as<std::string>(), "world");
}

TEST(FieldTests, ConstructVectorInt32) {
  std::vector<int32_t> v{1, 2, 3};
  field f{v};
  using vi32 = std::vector<int32_t>;
  EXPECT_TRUE(f.is<vi32>());
  EXPECT_EQ(f.as<vi32>(), v);
}

TEST(FieldTests, ConstructVectorFloat) {
  std::vector<float> v{1.0f, 2.0f};
  field f{v};
  using vf = std::vector<float>;
  EXPECT_TRUE(f.is<vf>());
}

TEST(FieldTests, ConstructVectorBool) {
  std::vector<bool> v{true, false, true};
  field f{v};
  using vb = std::vector<bool>;
  EXPECT_TRUE(f.is<vb>());
}

TEST(FieldTests, AssignMonostateFieldAcceptsAnyType) {
  field f; // monostate
  f = int32_t{7};
  EXPECT_EQ(f.as<int32_t>(), 7);
}

TEST(FieldTests, AssignSameTypeSucceeds) {
  field f{int32_t{1}};
  f = int32_t{2};
  EXPECT_EQ(f.as<int32_t>(), 2);
}

TEST(FieldTests, AssignWrongTypeThrows) {
  field f{int32_t{1}};
  EXPECT_THROW(f = float{1.0f}, std::runtime_error);
}

TEST(FieldTests, AsDefaultInitializesMonostate) {
  field f;
  int32_t& v = f.as<int32_t>(99);
  EXPECT_EQ(v, 99);
  EXPECT_TRUE(f.is<int32_t>());
}

TEST(FieldTests, AsWrongTypeThrows) {
  field f{int32_t{5}};
  EXPECT_THROW(f.as<float>(), std::runtime_error);
}

TEST(FieldTests, ImplicitConversionToCorrectType) {
  field f{int32_t{123}};
  int32_t v = static_cast<int32_t>(f);
  EXPECT_EQ(v, 123);
}

TEST(FieldTests, ImplicitConversionIncompatibleTypeThrows) {
  field f{int32_t{1}};
  EXPECT_THROW(f.get_as<dynamic_ptr>(), std::runtime_error);
}

// ─────────────────────────────────────────────────────────────────────────────
// Cross-type casting tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(FieldTests, CrossTypeCastIntToFloat) {
  field f{int32_t{42}};
  float v = static_cast<float>(f);
  EXPECT_FLOAT_EQ(v, 42.0f);
}

TEST(FieldTests, CrossTypeCastFloatToInt) {
  field f{3.7f};
  int32_t v = static_cast<int32_t>(f);
  EXPECT_EQ(v, 3);
}

TEST(FieldTests, CrossTypeCastIntToStringViaGetAs) {
  field f{int32_t{99}};
  EXPECT_EQ(f.get_as<std::string>(), "99");
}

TEST(FieldTests, CrossTypeCastFloatToStringViaGetAs) {
  field f{1.5f};
  std::string s = f.get_as<std::string>();
  EXPECT_FALSE(s.empty());
}

TEST(FieldTests, CrossTypeCastBoolToString) {
  field ft{true};
  field ff{false};
  EXPECT_EQ(ft.get_as<std::string>(), "true");
  EXPECT_EQ(ff.get_as<std::string>(), "false");
}

TEST(FieldTests, CrossTypeCastStringToInt) {
  field f{std::string{"42"}};
  EXPECT_EQ(f.get_as<int32_t>(), 42);
}

TEST(FieldTests, CrossTypeCastStringToFloat) {
  field f{std::string{"3.14"}};
  EXPECT_FLOAT_EQ(f.get_as<float>(), 3.14f);
}

TEST(FieldTests, CrossTypeCastMonostateThrows) {
  field f;
  EXPECT_THROW(f.get_as<int32_t>(), std::runtime_error);
}

TEST(FieldTests, CrossTypeCastIncompatibleThrows) {
  field f{int32_t{1}};
  EXPECT_THROW(f.get_as<dynamic_ptr>(), std::runtime_error);
}

TEST(FieldTests, CrossTypeCastAsStillStrict) {
  field f{int32_t{5}};
  EXPECT_THROW(f.as<float>(), std::runtime_error);
}

// ─────────────────────────────────────────────────────────────────────────────
// Custom attribute types used in attribute tests
// ─────────────────────────────────────────────────────────────────────────────
class TestRequired : public attribute {};
class MaxLength : public attribute {
 public:
  explicit MaxLength(size_t n) : max(n) {}
  size_t max;
};

TEST(FieldTests, AttributeAttachAndFind) {
  field f{std::string{"hi"}, attr<TestRequired>(), attr<MaxLength>(10)};
  EXPECT_NE(f.findAttribute<TestRequired>(), nullptr);
  const MaxLength* ml = f.findAttribute<MaxLength>();
  ASSERT_NE(ml, nullptr);
  EXPECT_EQ(ml->max, 10u);
}

TEST(FieldTests, AttributeNotFoundReturnsNullptr) {
  field f{int32_t{1}};
  EXPECT_EQ(f.findAttribute<TestRequired>(), nullptr);
}

TEST(FieldTests, AttributesSurviveCloneViaField) {
  field f{true, attr<TestRequired>()};
  field g = f; // copy
  EXPECT_NE(g.findAttribute<TestRequired>(), nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// field serialization round-trips
// ─────────────────────────────────────────────────────────────────────────────
static field field_roundtrip(const field& f) {
  std::stringstream ss;
  {
    stream_serializer out{ss};
    f.serialize(out);
  }
  stream_deserializer in{ss};
  return field::deserialize(in);
}

TEST(FieldSerializationTests, Monostate) {
  field f;
  field g = field_roundtrip(f);
  EXPECT_TRUE(g.is<std::monostate>());
}

TEST(FieldSerializationTests, Bool) {
  field g = field_roundtrip(field{false});
  EXPECT_TRUE(g.is<bool>());
  EXPECT_FALSE(g.as<bool>());
}

TEST(FieldSerializationTests, Int32) {
  field g = field_roundtrip(field{int32_t{-999}});
  EXPECT_EQ(g.as<int32_t>(), -999);
}

TEST(FieldSerializationTests, Float) {
  field g = field_roundtrip(field{1.23f});
  EXPECT_FLOAT_EQ(g.as<float>(), 1.23f);
}

TEST(FieldSerializationTests, String) {
  field g = field_roundtrip(field{std::string{"bison rocks"}});
  EXPECT_EQ(g.as<std::string>(), "bison rocks");
}

TEST(FieldSerializationTests, VectorInt32) {
  std::vector<int32_t> v{10, 20, 30};
  field g = field_roundtrip(field{v});
  EXPECT_EQ(g.as<std::vector<int32_t>>(), v);
}

TEST(FieldSerializationTests, VectorFloat) {
  std::vector<float> v{1.1f, 2.2f, 3.3f};
  field g = field_roundtrip(field{v});
  const auto& out = g.as<std::vector<float>>();
  ASSERT_EQ(out.size(), v.size());
  for (size_t i = 0; i < v.size(); ++i)
    EXPECT_FLOAT_EQ(out[i], v[i]);
}

TEST(FieldSerializationTests, VectorBool) {
  std::vector<bool> v{true, false, true, true};
  field g = field_roundtrip(field{v});
  EXPECT_EQ(g.as<std::vector<bool>>(), v);
}

TEST(FieldSerializationTests, NestedDynamic) {
  // A field holding a nested dynamic object.
  dynamic_ptr inner{"Inner"_key, {{"val"_key, int32_t{7}}}};
  field f{dynamic_ptr{inner}};
  field g = field_roundtrip(f);

  ASSERT_TRUE(g.is<dynamic_ptr>());
  auto nested = g.as<dynamic_ptr>();
  ASSERT_NE(nested, nullptr);
  EXPECT_EQ((*nested)["val"_key].as<int32_t>(), 7);
}

TEST(FieldSerializationTests, NullDynamicPtr) {
  field f{std::shared_ptr<dynamic>{}};
  field g = field_roundtrip(f);
  ASSERT_TRUE(g.is<dynamic_ptr>());
  EXPECT_EQ(g.as<dynamic_ptr>(), nullptr);
}

// ═════════════════════════════════════════════════════════════════════════════
// 5. dynamic – basic operations
// ═════════════════════════════════════════════════════════════════════════════

TEST(DynamicTests, DefaultConstructionHasClassField) {
  dynamic obj;
  // CLASS is always set; default is 0
  bool has_key = obj["__class"_key].is<bison_key_t>();
  EXPECT_TRUE(has_key);
}

TEST(DynamicTests, ConstructWithNamedClass) {
  dynamic obj{"Point"_key};
  bison_key_t klass = obj["__class"_key].as<bison_key_t>();
  EXPECT_EQ(klass.id, hash_t("Point"_key));
}

TEST(DynamicTests, SetAndGetNamedField) {
  dynamic obj;
  obj["x"_key] = int32_t{10};
  EXPECT_EQ(obj["x"_key].as<int32_t>(), 10);
}

TEST(DynamicTests, SetAndGetNumericIndex) {
  dynamic obj;
  obj[0] = std::string{"first"};
  obj[1] = std::string{"second"};
  EXPECT_EQ(obj[0].as<std::string>(), "first");
  EXPECT_EQ(obj[1].as<std::string>(), "second");
}

TEST(DynamicTests, SizeReflectsNumericFields) {
  dynamic obj;
  EXPECT_EQ(obj.size(), 0u);
  obj[0] = int32_t{0};
  EXPECT_EQ(obj.size(), 1u);
  obj[2] = int32_t{2}; // sparse: index 1 is missing but size is 3
  EXPECT_EQ(obj.size(), 3u);
}

TEST(DynamicTests, SizeIgnoresNamedFields) {
  dynamic obj{"MyClass"_key, {{"name"_key, std::string{"Alice"}}}};
  // Only named fields; no numeric keys → size = 0
  EXPECT_EQ(obj.size(), 0u);
}

TEST(DynamicTests, ClearRemovesOnlyNumericKeys) {
  dynamic obj;
  obj[0] = int32_t{0};
  obj[1] = int32_t{1};
  obj["name"_key] = std::string{"keep"};
  obj.clear();
  EXPECT_EQ(obj.size(), 0u);
  // named field must still be accessible
  EXPECT_EQ(obj["name"_key].as<std::string>(), "keep");
}

TEST(DynamicTests, EraseNumericIndex) {
  dynamic obj;
  obj[0] = int32_t{42};
  EXPECT_TRUE(obj.erase(0));
  EXPECT_FALSE(obj.erase(0)); // already gone
}

TEST(DynamicTests, EmptyAfterDefaultConstruct) {
  // Just CLASS field inserted by constructor.
  dynamic obj;
  obj["x"_key]; // access (creates empty field)
  EXPECT_FALSE(obj.empty());
}

TEST(DynamicTests, CloneIsIndependent) {
  dynamic orig{"Clone"_key, {{"v"_key, int32_t{1}}}};
  dynamic copy = orig.clone();
  copy["v"_key] = int32_t{2};
  EXPECT_EQ(orig["v"_key].as<int32_t>(), 1);
  EXPECT_EQ(copy["v"_key].as<int32_t>(), 2);
}

TEST(DynamicTests, CloneIsDeep) {
  auto inner = std::make_shared<dynamic>(0U);
  (*inner)["x"_key] = int32_t{10};

  dynamic orig;
  orig["child"_key] = dynamic_ptr{inner};

  dynamic copy = orig.clone();

  // Mutating the clone's nested object must not affect the original.
  copy["child"_key].as<dynamic_ptr>()->at("x"_key) = int32_t{99};
  EXPECT_EQ((*inner)["x"_key].as<int32_t>(), 10);
  EXPECT_EQ(copy["child"_key].as<dynamic_ptr>()->at("x"_key).as<int32_t>(), 99);
}

TEST(DynamicTests, CloneOriginalDoesNotAffectClone) {
  auto inner = std::make_shared<dynamic>(0U);
  (*inner)["x"_key] = int32_t{10};

  dynamic orig;
  orig["v"_key] = int32_t{1};
  orig["child"_key] = dynamic_ptr{inner};

  dynamic copy = orig.clone();

  // Mutating a scalar field on the original must not affect the clone.
  orig["v"_key] = int32_t{42};
  EXPECT_EQ(copy["v"_key].as<int32_t>(), 1);

  // Mutating the original's nested object must not affect the clone.
  (*inner)["x"_key] = int32_t{99};
  EXPECT_EQ(copy["child"_key].as<dynamic_ptr>()->at("x"_key).as<int32_t>(), 10);
}

TEST(DynamicTests, AddFieldReturnsFalseOnDuplicate) {
  dynamic obj;
  EXPECT_TRUE(obj.addField("x"_key, field{int32_t{1}}));
  EXPECT_FALSE(obj.addField("x"_key, field{int32_t{2}})); // already exists
  EXPECT_EQ(obj["x"_key].as<int32_t>(), 1);
}

TEST(DynamicTests, AtAliasesSubscriptOperator) {
  dynamic obj;
  obj["k"_key] = int32_t{5};
  EXPECT_EQ(obj.at("k"_key).as<int32_t>(), 5);
  obj[3] = int32_t{99};
  EXPECT_EQ(obj.at(3).as<int32_t>(), 99);
}

TEST(DynamicTests, NestedDynamic) {
  dynamic_ptr inner{0U, {{"depth"_key, int32_t{1}}}};
  dynamic outer;
  outer["child"_key] = dynamic_ptr{inner};
  auto child = outer["child"_key].as<dynamic_ptr>();
  ASSERT_NE(child, nullptr);
  EXPECT_EQ((*child)["depth"_key].as<int32_t>(), 1);
}

// ═════════════════════════════════════════════════════════════════════════════
// 6. dynamic serialization round-trips
// ═════════════════════════════════════════════════════════════════════════════

static dynamic dynamic_roundtrip(const dynamic& d) {
  std::stringstream ss;
  {
    stream_serializer out{ss};
    d.serialize(out);
  }
  stream_deserializer in{ss};
  return dynamic::deserialize(in);
}

TEST(DynamicSerializationTests, BasicFields) {
  dynamic src{"Ser"_key};
  src["a"_key] = int32_t{42};
  src["b"_key] = std::string{"hello"};
  src["c"_key] = true;

  auto dst = dynamic_roundtrip(src);
  EXPECT_EQ(dst["a"_key].as<int32_t>(), 42);
  EXPECT_EQ(dst["b"_key].as<std::string>(), "hello");
  EXPECT_TRUE(dst["c"_key].as<bool>());
}

TEST(DynamicSerializationTests, FloatField) {
  dynamic src;
  src["pi"_key] = 3.14f;
  auto dst = dynamic_roundtrip(src);
  EXPECT_FLOAT_EQ(dst["pi"_key].as<float>(), 3.14f);
}

TEST(DynamicSerializationTests, VectorFields) {
  dynamic src;
  src["ints"_key] = std::vector<int32_t>{1, 2, 3};
  src["floats"_key] = std::vector<float>{1.f, 2.f};
  src["bools"_key] = std::vector<bool>{true, false};

  auto dst = dynamic_roundtrip(src);
  EXPECT_EQ(dst["ints"_key].as<std::vector<int32_t>>(), (std::vector<int32_t>{1, 2, 3}));
}

TEST(DynamicSerializationTests, IndexedElements) {
  dynamic src;
  src[0] = int32_t{10};
  src[1] = int32_t{20};
  src[2] = int32_t{30};

  auto dst = dynamic_roundtrip(src);
  EXPECT_EQ(dst[0].as<int32_t>(), 10);
  EXPECT_EQ(dst[1].as<int32_t>(), 20);
  EXPECT_EQ(dst[2].as<int32_t>(), 30);
}

TEST(DynamicSerializationTests, NestedDynamic) {
  dynamic inner;
  inner["x"_key] = int32_t{7};
  dynamic src;
  src["child"_key] = dynamic_ptr{std::move(inner)};

  auto dst = dynamic_roundtrip(src);
  auto child = dst["child"_key].as<dynamic_ptr>();
  ASSERT_NE(child, nullptr);
  EXPECT_EQ((*child)["x"_key].as<int32_t>(), 7);
}

TEST(DynamicSerializationTests, NullNestedPointer) {
  dynamic src;
  src["ptr"_key] = std::shared_ptr<dynamic>{};
  auto dst = dynamic_roundtrip(src);
  auto ptr = dst["ptr"_key].as<dynamic_ptr>();
  EXPECT_EQ(ptr, nullptr);
}

// ═════════════════════════════════════════════════════════════════════════════
// 7. dynamic methods
// ═════════════════════════════════════════════════════════════════════════════

TEST(DynamicMethodTests, AddAndCallMethod) {
  dynamic obj;
  obj.addMethod("greet"_key, method{[](dynamic& self, const dynamic& params) -> dynamic {
                  dynamic result;
                  result["msg"_key] = std::string{"hello"};
                  return result;
                }});
  dynamic result = obj.call("greet"_key, dynamic{});
  EXPECT_EQ(result["msg"_key].as<std::string>(), "hello");
}

TEST(DynamicMethodTests, MethodReceivesParams) {
  dynamic obj;
  obj.addMethod("add"_key, method{[](dynamic& self, const dynamic& params) -> dynamic {
                  int32_t a = params["a"_key].as<int32_t>();
                  int32_t b = params["b"_key].as<int32_t>();
                  dynamic result;
                  result["sum"_key] = a + b;
                  return result;
                }});

  dynamic args;
  args["a"_key] = int32_t{3};
  args["b"_key] = int32_t{4};
  dynamic result = obj.call("add"_key, args);
  EXPECT_EQ(result["sum"_key].as<int32_t>(), 7);
}

TEST(DynamicMethodTests, MethodCanMutateSelf) {
  dynamic obj;
  obj["counter"_key] = int32_t{0};
  obj.addMethod("inc"_key, method{[](dynamic& self, const dynamic& params) -> dynamic {
                  self["counter"_key] = int32_t{self["counter"_key].as<int32_t>() + 1};
                  return dynamic{};
                }});
  obj.call("inc"_key, dynamic{});
  obj.call("inc"_key, dynamic{});
  EXPECT_EQ(obj["counter"_key].as<int32_t>(), 2);
}

TEST(DynamicMethodTests, CallNonexistentMethodThrows) {
  dynamic obj;
  EXPECT_THROW(obj.call("missing"_key, dynamic{}), std::runtime_error);
}

TEST(DynamicMethodTests, AddMethodReturnsFalseOnDuplicate) {
  dynamic obj;
  method_fn fn = [](dynamic&, const dynamic&) -> dynamic { return {}; };
  EXPECT_TRUE(obj.addMethod("fn"_key, method{fn}));
  EXPECT_FALSE(obj.addMethod("fn"_key, method{fn}));
}

// ═════════════════════════════════════════════════════════════════════════════
// 8. Class hierarchy and inheritance
// ═════════════════════════════════════════════════════════════════════════════

class InheritanceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    clearClassRegistry();
  }
  void TearDown() override {
    clearClassRegistry();
  }
};

TEST_F(InheritanceTest, AddClassSucceeds) {
  auto klass = dynamic_ptr{"Animal"_key, {{"legs"_key, int32_t{4}}}};
  EXPECT_TRUE(dynamic::addClass(0U, klass, 0U));
}

TEST_F(InheritanceTest, AddDuplicateClassFails) {
  auto k1 = dynamic_ptr{"Cat"_key};
  auto k2 = dynamic_ptr{"Cat"_key};
  EXPECT_TRUE(dynamic::addClass(0U, k1, 0U));
  EXPECT_FALSE(dynamic::addClass(0U, k2, 0U));
}

TEST_F(InheritanceTest, CircularInheritanceIsRejected) {
  auto a = dynamic_ptr{"A"_key};
  auto b = dynamic_ptr{"B"_key};
  EXPECT_TRUE(dynamic::addClass(0U, a, 0U));
  EXPECT_TRUE(dynamic::addClass(0U, b, "A"_key));
  // Trying to make A a child of B would create a cycle A→B→A
  auto a2 = dynamic_ptr{"A"_key};
  EXPECT_FALSE(dynamic::addClass(0U, a2, "B"_key));
}

TEST_F(InheritanceTest, FieldInheritedFromParent) {
  auto base = dynamic_ptr{"Vehicle"_key, {{"wheels"_key, int32_t{4}}}};
  ASSERT_TRUE(dynamic::addClass(0U, base, 0U));

  auto child = dynamic_ptr{"Car"_key};
  ASSERT_TRUE(dynamic::addClass(0U, child, "Vehicle"_key));

  dynamic car = dynamic::instantiate("Car"_key);

  // Direct findField check (bypasses operator[])
  auto* fw = car.findField("wheels"_key);
  ASSERT_NE(fw, nullptr) << "findField returned nullptr for 'wheels'";
  ASSERT_TRUE(fw->is<int32_t>()) << "wheels field is not int32_t";
  EXPECT_EQ(fw->as<int32_t>(), 4);

  // "wheels" comes from Vehicle; should be inherited
  EXPECT_EQ(car["wheels"_key].as<int32_t>(), 4);
}

TEST_F(InheritanceTest, MethodInheritedFromParent) {
  auto base = dynamic_ptr{"Animal2"_key};
  base->addMethod("speak"_key, method{[](dynamic& self, const dynamic& params) -> dynamic {
                    dynamic r;
                    r["sound"_key] = std::string{"..."};
                    return r;
                  }});
  dynamic::addClass(0U, base, 0U);

  auto child = dynamic_ptr{"Dog"_key};
  dynamic::addClass(0U, child, "Animal2"_key);

  dynamic dog = dynamic::instantiate("Dog"_key);
  dynamic res = dog.call("speak"_key, dynamic{});
  EXPECT_EQ(res["sound"_key].as<std::string>(), "...");
}

TEST_F(InheritanceTest, DerivedClassOverridesParentField) {
  auto base = dynamic_ptr{"Base2"_key, {{"val"_key, int32_t{1}}}};
  ASSERT_TRUE(dynamic::addClass(0U, base, 0U));

  auto derived = dynamic_ptr{"Derived2"_key, {{"val"_key, int32_t{2}}}};
  ASSERT_TRUE(dynamic::addClass(0U, derived, "Base2"_key));

  dynamic d = dynamic::instantiate("Derived2"_key);
  // Warm the inheritance cache via a direct findField call, then verify
  // both access paths agree on val=2 (derived class takes precedence).
  auto* fv = d.findField("val"_key);
  ASSERT_NE(fv, nullptr);
  EXPECT_EQ(fv->as<int32_t>(), 2);
  EXPECT_EQ(d["val"_key].as<int32_t>(), 2);
}

TEST_F(InheritanceTest, MultiLevelFieldInheritance) {
  dynamic::addClass(0U, dynamic_ptr{"L0"_key, {{"depth"_key, int32_t{0}}}}, 0U);
  dynamic::addClass(0U, dynamic_ptr{"L1"_key}, "L0"_key);
  dynamic::addClass(0U, dynamic_ptr{"L2"_key}, "L1"_key);

  dynamic obj = dynamic::instantiate("L2"_key);
  EXPECT_EQ(obj["depth"_key].as<int32_t>(), 0);
}

TEST_F(InheritanceTest, FindClassSearchesHierarchy) {
  dynamic::addClass(0U, dynamic_ptr{"Root"_key}, 0U);
  dynamic::addClass(0U, dynamic_ptr{"Child"_key}, "Root"_key);

  dynamic obj = dynamic::instantiate("Child"_key);
  EXPECT_NE(obj.findClass("Root"_key), nullptr);
  EXPECT_NE(obj.findClass("Child"_key), nullptr);
  EXPECT_EQ(obj.findClass("Missing"_key), nullptr);
}

TEST_F(InheritanceTest, InstantiateCreatesCorrectClass) {
  dynamic obj = dynamic::instantiate("MyClass"_key);
  bison_key_t k = obj["__class"_key].as<bison_key_t>();
  EXPECT_EQ(k.id, hash_t("MyClass"_key));
}

// ═════════════════════════════════════════════════════════════════════════════
// 9. Template-based serialization
// ═════════════════════════════════════════════════════════════════════════════

class TemplateSerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    clearClassRegistry();
  }
  void TearDown() override {
    clearClassRegistry();
  }
};

TEST_F(TemplateSerTest, RoundtripWithSingleClass) {
  // Register a prototype with two fields.
  dynamic::addClass(0U, dynamic_ptr{"Pt"_key, {{"x"_key, int32_t{0}}, {"y"_key, int32_t{0}}}}, 0U);

  // Create an instance and populate it.
  dynamic pt = dynamic::instantiate("Pt"_key);
  pt["x"_key] = int32_t{3};
  pt["y"_key] = int32_t{7};

  std::stringstream ss;
  {
    stream_serializer out{ss};
    pt.serializeWithSchema(out);
  }

  stream_deserializer in{ss};
  auto restored = dynamic::deserializeWithSchema(in);
  EXPECT_EQ(restored["x"_key].as<int32_t>(), 3);
  EXPECT_EQ(restored["y"_key].as<int32_t>(), 7);
}

// ═════════════════════════════════════════════════════════════════════════════
// 10. userdata
// ═════════════════════════════════════════════════════════════════════════════

struct AppContext : public userdata {
  int session_id = 42;
};

TEST(UserdataTests, SetAndGet) {
  dynamic obj;
  obj.setUserdata(std::make_shared<AppContext>());
  auto ctx = std::dynamic_pointer_cast<AppContext>(obj.getUserdata());
  ASSERT_NE(ctx, nullptr);
  EXPECT_EQ(ctx->session_id, 42);
}

TEST(UserdataTests, DefaultIsNull) {
  dynamic obj;
  EXPECT_EQ(obj.getUserdata(), nullptr);
}

TEST(UserdataTests, UserdataNotIncludedInSerialization) {
  dynamic src;
  src["v"_key] = int32_t{1};
  src.setUserdata(std::make_shared<AppContext>());

  auto dst = dynamic_roundtrip(src);
  // Value round-trips correctly...
  EXPECT_EQ(dst["v"_key].as<int32_t>(), 1);
  // ...but userdata is NOT restored.
  EXPECT_EQ(dst.getUserdata(), nullptr);
}

// ═════════════════════════════════════════════════════════════════════════════
// 11. dynamic_ptr convenience constructors
// ═════════════════════════════════════════════════════════════════════════════

TEST(DynamicPtrTests, DefaultConstructionIsNotNull) {
  dynamic_ptr p;
  EXPECT_NE(p.get(), nullptr);
}

TEST(DynamicPtrTests, ConstructWithClassAndFields) {
  dynamic_ptr p{"MyObj"_key, {{"v"_key, int32_t{5}}}};
  EXPECT_EQ((*p)["v"_key].as<int32_t>(), 5);
}

TEST(DynamicPtrTests, ConstructFromRvalue) {
  dynamic obj{"Rval"_key};
  obj["x"_key] = int32_t{99};
  dynamic_ptr p{std::move(obj)};
  EXPECT_EQ((*p)["x"_key].as<int32_t>(), 99);
}

TEST(DynamicPtrTests, CompatibleWithSharedPtr) {
  dynamic_ptr dp{"T"_key};
  dynamic_ptr sp = dp;
  EXPECT_EQ(sp.get(), dp.get());
}

// ═════════════════════════════════════════════════════════════════════════════
// 12. JSON extension
// ═════════════════════════════════════════════════════════════════════════════

TEST(ExtensionsTests, ParseFlatObject) {
  auto obj = extensions::from_json(R"({"x": 1, "msg": "hi", "flag": true})");
  ASSERT_NE(obj, nullptr);
  EXPECT_EQ((*obj)["x"].as<int32_t>(), 1);
  EXPECT_EQ((*obj)["msg"].as<std::string>(), "hi");
  EXPECT_TRUE((*obj)["flag"].as<bool>());
}

TEST(ExtensionsTests, ParseNestedObject) {
  auto obj = extensions::from_json(R"({"a": {"b": 42}})");
  ASSERT_NE(obj, nullptr);
  auto inner = (*obj)["a"].as<dynamic_ptr>();
  ASSERT_NE(inner, nullptr);
  EXPECT_EQ((*inner)["b"].as<int32_t>(), 42);
}

TEST(ExtensionsTests, ParseFloat) {
  auto obj = extensions::from_json(R"({"pi": 3.14})");
  ASSERT_NE(obj, nullptr);
  EXPECT_NEAR((*obj)["pi"].as<float>(), 3.14f, 0.001f);
}

TEST(ExtensionsTests, ParseNullValue) {
  auto obj = extensions::from_json(R"({"ptr": null})");
  ASSERT_NE(obj, nullptr);
  auto ptr = (*obj)["ptr"].as<dynamic_ptr>();
  EXPECT_EQ(ptr, nullptr);
}

TEST(ExtensionsTests, ParseHomogeneousIntArrayAsVectorInt32) {
  // A homogeneous integer array reconstructs as std::vector<int32_t>,
  // mirroring how bison_to_json renders that field type.
  auto obj = extensions::from_json(R"({"items": [10, 20, 30]})");
  ASSERT_NE(obj, nullptr);
  EXPECT_EQ((*obj)["items"].as<std::vector<int32_t>>(), (std::vector<int32_t>{10, 20, 30}));
}

TEST(ExtensionsTests, ParseHomogeneousBoolArrayAsVectorBool) {
  auto obj = extensions::from_json(R"({"flags": [true, false, true]})");
  ASSERT_NE(obj, nullptr);
  EXPECT_EQ((*obj)["flags"].as<std::vector<bool>>(), (std::vector<bool>{true, false, true}));
}

TEST(ExtensionsTests, ParseArrayWithFloatAsVectorFloat) {
  // Mixed integer/float numeric arrays coerce to std::vector<float>, since
  // that is what bison_to_json emits for a float field.
  auto obj = extensions::from_json(R"({"items": [1, 2.5, 3]})");
  ASSERT_NE(obj, nullptr);
  const auto& v = (*obj)["items"].as<std::vector<float>>();
  ASSERT_EQ(v.size(), 3u);
  EXPECT_FLOAT_EQ(v[0], 1.0f);
  EXPECT_FLOAT_EQ(v[1], 2.5f);
  EXPECT_FLOAT_EQ(v[2], 3.0f);
}

TEST(ExtensionsTests, ParseMixedTypeArrayAsIndexedDynamic) {
  // Arrays that mix incompatible element types (e.g. numbers and strings)
  // cannot be a vector<T>, so they fall back to an indexed dynamic_ptr.
  auto obj = extensions::from_json(R"({"items": [10, "twenty", 30]})");
  ASSERT_NE(obj, nullptr);
  auto arr = (*obj)["items"].as<dynamic_ptr>();
  ASSERT_NE(arr, nullptr);
  EXPECT_EQ((*arr)[0].as<int32_t>(), 10);
  EXPECT_EQ((*arr)[1].as<std::string>(), "twenty");
  EXPECT_EQ((*arr)[2].as<int32_t>(), 30);
  EXPECT_EQ(arr->size(), 3u);
}

TEST(ExtensionsTests, JsonRoundTripVectorFields) {
  dynamic src;
  src["ints"_key] = std::vector<int32_t>{1, 2, 3};
  src["floats"_key] = std::vector<float>{1.5f, 2.5f};
  src["bools"_key] = std::vector<bool>{true, false, true};

  const std::string text = extensions::to_json(src, {{hash("ints"), "ints"}, {hash("floats"), "floats"}, {hash("bools"), "bools"}});
  auto dst = extensions::from_json(text);
  ASSERT_NE(dst, nullptr);

  EXPECT_EQ((*dst)["ints"].as<std::vector<int32_t>>(), (std::vector<int32_t>{1, 2, 3}));
  EXPECT_EQ((*dst)["bools"].as<std::vector<bool>>(), (std::vector<bool>{true, false, true}));
  const auto& floats = (*dst)["floats"].as<std::vector<float>>();
  ASSERT_EQ(floats.size(), 2u);
  EXPECT_FLOAT_EQ(floats[0], 1.5f);
  EXPECT_FLOAT_EQ(floats[1], 2.5f);
}

TEST(ExtensionsTests, ModifyFieldAfterJsonImport) {
  auto obj = extensions::from_json(R"({"name": "original"})");
  (*obj)["name"] = std::string{"modified"};
  EXPECT_EQ((*obj)["name"].as<std::string>(), "modified");
}

// ═════════════════════════════════════════════════════════════════════════════
// 13. field index_of (compile-time utility)
// ═════════════════════════════════════════════════════════════════════════════

TEST(FieldIndexOfTests, MonostateIsIndex0) {
  constexpr auto idx = field::index_of<std::monostate>();
  EXPECT_EQ(idx, 0u);
}

TEST(FieldIndexOfTests, BoolIsIndex3) {
  constexpr auto idx = field::index_of<bool>();
  EXPECT_EQ(idx, 3u);
}

TEST(FieldIndexOfTests, Int32IsIndex4) {
  constexpr auto idx = field::index_of<int32_t>();
  EXPECT_EQ(idx, 4u);
}

TEST(FieldIndexOfTests, FloatIsIndex5) {
  constexpr auto idx = field::index_of<float>();
  EXPECT_EQ(idx, 5u);
}

TEST(FieldIndexOfTests, StringIsIndex7) {
  constexpr auto idx = field::index_of<std::string>();
  EXPECT_EQ(idx, 7u);
}

TEST(FieldIndexOfTests, VectorBoolIsIndex8) {
  using vb = std::vector<bool>;
  constexpr auto idx = field::index_of<vb>();
  EXPECT_EQ(idx, 8u);
}

TEST(FieldIndexOfTests, VectorInt32IsIndex9) {
  using vi32 = std::vector<int32_t>;
  constexpr auto idx = field::index_of<vi32>();
  EXPECT_EQ(idx, 9u);
}

TEST(FieldIndexOfTests, VectorFloatIsIndex10) {
  using vf = std::vector<float>;
  constexpr auto idx = field::index_of<vf>();
  EXPECT_EQ(idx, 10u);
}

// ═════════════════════════════════════════════════════════════════════════════
// 14. YAML import (extensions::from_yaml)
// ═════════════════════════════════════════════════════════════════════════════

TEST(YamlTests, FlatMapping) {
  auto obj = extensions::from_yaml("x: 1\ny: 2.5\nname: alice\n");
  ASSERT_NE(obj, nullptr);
  EXPECT_EQ((*obj)["x"].as<int32_t>(), 1);
  EXPECT_FLOAT_EQ((*obj)["y"].as<float>(), 2.5f);
  EXPECT_EQ((*obj)["name"].as<std::string>(), "alice");
}

TEST(YamlTests, BooleanValues) {
  auto obj = extensions::from_yaml("a: true\nb: false\nc: yes\nd: no\n");
  ASSERT_NE(obj, nullptr);
  EXPECT_TRUE(bool((*obj)["a"]));
  EXPECT_FALSE(bool((*obj)["b"]));
  EXPECT_TRUE(bool((*obj)["c"]));
  EXPECT_FALSE(bool((*obj)["d"]));
}

TEST(YamlTests, NullValue) {
  auto obj = extensions::from_yaml("key: null\n");
  ASSERT_NE(obj, nullptr);
  dynamic_ptr sp = (*obj)["key"];
  EXPECT_EQ(sp, nullptr);
}

TEST(YamlTests, SequenceTopLevel) {
  auto obj = extensions::from_yaml("- 1\n- 2\n- 3\n");
  ASSERT_NE(obj, nullptr);
  EXPECT_EQ(obj->size(), 3u);
  EXPECT_EQ((*obj)[size_t{0}].as<int32_t>(), 1);
  EXPECT_EQ((*obj)[size_t{1}].as<int32_t>(), 2);
  EXPECT_EQ((*obj)[size_t{2}].as<int32_t>(), 3);
}

TEST(YamlTests, NestedMapping) {
  auto obj = extensions::from_yaml("person:\n  name: bob\n  age: 30\n");
  ASSERT_NE(obj, nullptr);
  dynamic_ptr person = (*obj)["person"];
  ASSERT_NE(person, nullptr);
  EXPECT_EQ((*person)["name"].as<std::string>(), "bob");
  EXPECT_EQ((*person)["age"].as<int32_t>(), 30);
}

TEST(YamlTests, SequenceInsideMapping) {
  auto obj = extensions::from_yaml("items:\n  - apple\n  - banana\n");
  ASSERT_NE(obj, nullptr);
  dynamic_ptr items = (*obj)["items"];
  ASSERT_NE(items, nullptr);
  EXPECT_EQ(items->size(), 2u);
  EXPECT_EQ((*items)[size_t{0}].as<std::string>(), "apple");
  EXPECT_EQ((*items)[size_t{1}].as<std::string>(), "banana");
}

TEST(YamlTests, QuotedStringNotCoerced) {
  auto obj = extensions::from_yaml("val: \"42\"\n");
  ASSERT_NE(obj, nullptr);
  // A quoted scalar must remain a string even though "42" looks like an int.
  EXPECT_EQ((*obj)["val"].as<std::string>(), "42");
}

TEST(YamlTests, InvalidYamlThrows) {
  EXPECT_THROW(extensions::from_yaml("{broken"), std::runtime_error);
}

TEST(YamlTests, HomogeneousIntSequenceAsVectorInt32) {
  // A nested homogeneous integer sequence reconstructs as
  // std::vector<int32_t>, mirroring bison_to_yaml's rendering of that
  // field type.
  auto obj = extensions::from_yaml("items:\n  - 10\n  - 20\n  - 30\n");
  ASSERT_NE(obj, nullptr);
  EXPECT_EQ((*obj)["items"].as<std::vector<int32_t>>(), (std::vector<int32_t>{10, 20, 30}));
}

TEST(YamlTests, HomogeneousBoolSequenceAsVectorBool) {
  auto obj = extensions::from_yaml("flags:\n  - true\n  - false\n  - true\n");
  ASSERT_NE(obj, nullptr);
  EXPECT_EQ((*obj)["flags"].as<std::vector<bool>>(), (std::vector<bool>{true, false, true}));
}

TEST(YamlTests, MixedNumericSequenceAsVectorFloat) {
  auto obj = extensions::from_yaml("items:\n  - 1\n  - 2.5\n  - 3\n");
  ASSERT_NE(obj, nullptr);
  const auto& v = (*obj)["items"].as<std::vector<float>>();
  ASSERT_EQ(v.size(), 3u);
  EXPECT_FLOAT_EQ(v[0], 1.0f);
  EXPECT_FLOAT_EQ(v[1], 2.5f);
  EXPECT_FLOAT_EQ(v[2], 3.0f);
}

TEST(YamlTests, SequenceInsideMappingStillIndexedWhenNotHomogeneous) {
  // Strings cannot become a vector<T>, so this still falls back to an
  // indexed dynamic_ptr (same as SequenceInsideMapping above).
  auto obj = extensions::from_yaml("items:\n  - apple\n  - banana\n");
  ASSERT_NE(obj, nullptr);
  dynamic_ptr items = (*obj)["items"];
  ASSERT_NE(items, nullptr);
  EXPECT_EQ(items->size(), 2u);
}

TEST(YamlTests, TopLevelSequenceRemainsIndexedDynamic) {
  // bison_to_yaml always emits a mapping at the document root, so top-level
  // sequences are hand-authored YAML; they keep the pre-existing indexed
  // dynamic_ptr behavior rather than becoming a vector<T> field.
  auto obj = extensions::from_yaml("- 1\n- 2\n- 3\n");
  ASSERT_NE(obj, nullptr);
  EXPECT_EQ(obj->size(), 3u);
  EXPECT_EQ((*obj)[size_t{0}].as<int32_t>(), 1);
}

TEST(YamlTests, RoundTripVectorFields) {
  dynamic src;
  src["ints"_key] = std::vector<int32_t>{1, 2, 3};
  src["floats"_key] = std::vector<float>{1.5f, 2.5f};
  src["bools"_key] = std::vector<bool>{true, false, true};

  const std::string text = extensions::to_yaml(
      src, {{hash("ints"), "ints"}, {hash("floats"), "floats"}, {hash("bools"), "bools"}});
  auto dst = extensions::from_yaml(text);
  ASSERT_NE(dst, nullptr);

  EXPECT_EQ((*dst)["ints"].as<std::vector<int32_t>>(), (std::vector<int32_t>{1, 2, 3}));
  EXPECT_EQ((*dst)["bools"].as<std::vector<bool>>(), (std::vector<bool>{true, false, true}));
  const auto& floats = (*dst)["floats"].as<std::vector<float>>();
  ASSERT_EQ(floats.size(), 2u);
  EXPECT_FLOAT_EQ(floats[0], 1.5f);
  EXPECT_FLOAT_EQ(floats[1], 2.5f);
}

// ═════════════════════════════════════════════════════════════════════════════
// 16. Namespace support
// ═════════════════════════════════════════════════════════════════════════════

class NamespaceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    clearClassRegistry();
  }
  void TearDown() override {
    clearClassRegistry();
  }
};

TEST_F(NamespaceTest, SameClassNameInDifferentNamespacesSucceeds) {
  auto math_table = dynamic_ptr{"table"_key, {{"rows"_key, int32_t{10}}}};
  auto ikea_table = dynamic_ptr{"table"_key, {{"legs"_key, int32_t{4}}}};
  EXPECT_TRUE(dynamic::addClass("math"_key, math_table, 0U));
  EXPECT_TRUE(dynamic::addClass("ikea"_key, ikea_table, 0U));
}

TEST_F(NamespaceTest, DuplicateClassInSameNamespaceFails) {
  auto t1 = dynamic_ptr{"table"_key, {{"rows"_key, int32_t{0}}}};
  auto t2 = dynamic_ptr{"table"_key, {{"rows"_key, int32_t{1}}}};
  EXPECT_TRUE(dynamic::addClass("math"_key, t1, 0U));
  EXPECT_FALSE(dynamic::addClass("math"_key, t2, 0U));
}

TEST_F(NamespaceTest, PrototypeHasNamespaceFieldSet) {
  auto klass = dynamic_ptr{"Vec3"_key, {{"x"_key, float{0}}}};
  ASSERT_TRUE(dynamic::addClass("math"_key, klass, 0U));
  auto* nsField = klass->findField(dynamic::NAMESPACE);
  ASSERT_NE(nsField, nullptr);
  EXPECT_EQ(nsField->as<bison_key_t>().id, hash("math"));
}

TEST_F(NamespaceTest, InstantiateWithNamespaceResolvesCorrectFields) {
  // Two namespaces with a class named "table" but different fields.
  auto math_table = dynamic_ptr{"table"_key, {{"rows"_key, int32_t{10}}}};
  auto ikea_table = dynamic_ptr{"table"_key, {{"legs"_key, int32_t{4}}}};
  ASSERT_TRUE(dynamic::addClass("math"_key, math_table, 0U));
  ASSERT_TRUE(dynamic::addClass("ikea"_key, ikea_table, 0U));

  dynamic mt = dynamic::instantiate("math"_key, "table"_key);
  dynamic it = dynamic::instantiate("ikea"_key, "table"_key);

  EXPECT_EQ(mt["rows"_key].as<int32_t>(), 10);
  EXPECT_EQ(it["legs"_key].as<int32_t>(), 4);
}

TEST_F(NamespaceTest, CircularInheritanceCheckedWithinNamespace) {
  auto a = dynamic_ptr{"A"_key};
  auto b = dynamic_ptr{"B"_key};
  EXPECT_TRUE(dynamic::addClass("ns"_key, a, 0U));
  EXPECT_TRUE(dynamic::addClass("ns"_key, b, "A"_key));
  // Trying to make A a child of B (within "ns") creates a cycle.
  auto a2 = dynamic_ptr{"A"_key};
  EXPECT_FALSE(dynamic::addClass("ns"_key, a2, "B"_key));
}

TEST_F(NamespaceTest, InheritanceWorksWithinNamespace) {
  auto base = dynamic_ptr{"Furniture"_key, {{"material"_key, std::string{"wood"}}}};
  auto derived = dynamic_ptr{"Chair"_key, {{"backrest"_key, true}}};
  ASSERT_TRUE(dynamic::addClass("ikea"_key, base, 0U));
  ASSERT_TRUE(dynamic::addClass("ikea"_key, derived, "Furniture"_key));

  dynamic chair = dynamic::instantiate("ikea"_key, "Chair"_key);
  EXPECT_EQ(chair["material"_key].as<std::string>(), "wood");
  EXPECT_TRUE(chair["backrest"_key].as<bool>());
}

TEST_F(NamespaceTest, FindClassSearchesCorrectNamespace) {
  auto shape = dynamic_ptr{"Shape"_key};
  auto circle = dynamic_ptr{"Circle"_key};
  ASSERT_TRUE(dynamic::addClass("geo"_key, shape, 0U));
  ASSERT_TRUE(dynamic::addClass("geo"_key, circle, "Shape"_key));

  dynamic c = dynamic::instantiate("geo"_key, "Circle"_key);
  EXPECT_NE(c.findClass("Shape"_key), nullptr);
  EXPECT_NE(c.findClass("Circle"_key), nullptr);
  EXPECT_EQ(c.findClass("Missing"_key), nullptr);
}

TEST_F(NamespaceTest, GlobalNamespaceUsedWhenNoNamespaceGiven) {
  auto klass = dynamic_ptr{"Widget"_key, {{"size"_key, int32_t{1}}}};
  ASSERT_TRUE(dynamic::addClass(0U, klass, 0U));
  dynamic w = dynamic::instantiate("Widget"_key);
  EXPECT_EQ(w["size"_key].as<int32_t>(), 1);
}

TEST_F(NamespaceTest, AbsentNamespaceDoesNotSearchNamedNamespaces) {
  auto ns_only = dynamic_ptr{"WidgetNsOnly"_key, {{"size"_key, int32_t{7}}}};
  ASSERT_TRUE(dynamic::addClass("math"_key, ns_only, 0U));

  // No namespace on the instance means global namespace only.
  dynamic w = dynamic::instantiate("WidgetNsOnly"_key);
  EXPECT_EQ(w.findField("size"_key), nullptr);
}

TEST_F(NamespaceTest, SchemaSerializationRoundtripWithNamespace) {
  dynamic::addClass("math"_key, dynamic_ptr{"Pt"_key, {{"x"_key, int32_t{0}}, {"y"_key, int32_t{0}}}}, 0U);

  dynamic pt = dynamic::instantiate("math"_key, "Pt"_key);
  pt["x"_key] = int32_t{5};
  pt["y"_key] = int32_t{9};

  buffer_serializer out;
  pt.serializeWithSchema(out);

  buffer_deserializer in{out.buffer()};
  auto restored = dynamic::deserializeWithSchema(in);
  EXPECT_EQ(restored["x"_key].as<int32_t>(), 5);
  EXPECT_EQ(restored["y"_key].as<int32_t>(), 9);
}

TEST_F(InheritanceTest, SingleClassDirectField) {
  // Minimal: single class with a field, no namespace
  dynamic::addClass(0U, dynamic_ptr{"Widget42"_key, {{"size"_key, int32_t{1}}}}, 0U);
  dynamic w = dynamic::instantiate("Widget42"_key);
  auto* sf = w.findField("size"_key);
  EXPECT_NE(sf, nullptr) << "findField returned null";
  if (sf)
    EXPECT_EQ(sf->as<int32_t>(), 1) << "wrong value";
}

TEST_F(InheritanceTest, SingleClassOperatorBracket) {
  // Same as SingleClassDirectField but via operator[]
  dynamic::addClass(0U, dynamic_ptr{"Widget43"_key, {{"size"_key, int32_t{1}}}}, 0U);
  dynamic w = dynamic::instantiate("Widget43"_key);
  EXPECT_EQ(w["size"_key].as<int32_t>(), 1);
}

// ═════════════════════════════════════════════════════════════════════════════
// N. Pretty-print
// ═════════════════════════════════════════════════════════════════════════════

TEST(PrintTest, MultilineContainsFieldValues) {
  dynamic obj;
  obj["name"_key] = field{std::string{"Alice"}, attr<DisplayName>("name")};
  obj["age"_key] = field{int32_t{30}, attr<DisplayName>("age")};
  obj["active"_key] = field{bool{true}, attr<DisplayName>("active")};

  const std::string out = print(obj);
  EXPECT_NE(out.find("\"Alice\""), std::string::npos);
  EXPECT_NE(out.find("30"), std::string::npos);
  EXPECT_NE(out.find("true"), std::string::npos);
  // DisplayName attributes cause fields to print with their names.
  EXPECT_NE(out.find("name"), std::string::npos);
  EXPECT_NE(out.find("age"), std::string::npos);
  EXPECT_NE(out.find("active"), std::string::npos);
}

TEST(PrintTest, MultilineIsIndented) {
  dynamic obj;
  obj["x"_key] = field{int32_t{1}, attr<DisplayName>("x")};

  const std::string out = print(obj);
  // Multiline output should contain a newline and indent.
  EXPECT_NE(out.find('\n'), std::string::npos);
  EXPECT_NE(out.find("  x"), std::string::npos);
}

TEST(PrintTest, SingleLineHasNoBrokenLines) {
  dynamic obj;
  obj["score"_key] = field{float{9.5f}, attr<DisplayName>("score")};

  print_options opts;
  opts.multiline = false;
  const std::string out = print(obj, opts);
  EXPECT_EQ(out.find('\n'), std::string::npos);
  EXPECT_NE(out.find("9.5"), std::string::npos);
  EXPECT_NE(out.find('{'), std::string::npos);
  EXPECT_NE(out.find('}'), std::string::npos);
}

TEST(PrintTest, CustomIndentIsHonoured) {
  dynamic obj;
  obj["v"_key] = field{int32_t{42}, attr<DisplayName>("v")};

  print_options opts;
  opts.multiline = true;
  opts.indent = "\t";
  const std::string out = print(obj, opts);
  EXPECT_NE(out.find("\tv"), std::string::npos);
}

TEST(PrintTest, NestedObjectPrintsRecursively) {
  dynamic inner;
  inner["city"_key] = field{std::string{"Springfield"}, attr<DisplayName>("city")};

  dynamic outer;
  outer["addr"_key] = field{dynamic_ptr{std::make_shared<dynamic>(std::move(inner))}, attr<DisplayName>("addr")};

  const std::string out = print(outer);
  EXPECT_NE(out.find("\"Springfield\""), std::string::npos);
  EXPECT_NE(out.find("city"), std::string::npos);
  EXPECT_NE(out.find("addr"), std::string::npos);
}

TEST(PrintTest, ArrayFieldsAppearInOutput) {
  dynamic obj;
  field scores{std::vector<int32_t>{10, 20, 30}};
  obj["scores"_key] = std::move(scores);

  print_options opts;
  opts.multiline = false;
  const std::string out = print(obj, opts);
  EXPECT_NE(out.find("10"), std::string::npos);
  EXPECT_NE(out.find("20"), std::string::npos);
  EXPECT_NE(out.find("30"), std::string::npos);
}

TEST(PrintTest, MethodsAppearsInOutput) {
  dynamic obj;
  obj["value"_key] = field{int32_t{0}, attr<DisplayName>("value")};
  obj.addMethod(
      "reset"_key,
      method{
          [](dynamic&, const dynamic&) -> dynamic { return {}; },
          attr<DisplayName>("Reset"),
          attr<Description>("Resets to zero")});
  obj.addMethod("noop"_key, method{[](dynamic&, const dynamic&) -> dynamic { return {}; }});

  const std::string out = print(obj);
  // Method with DisplayName uses that name as the key.
  EXPECT_NE(out.find("Reset"), std::string::npos);
  // Method attrs summary appears in value.
  EXPECT_NE(out.find("displayName"), std::string::npos);
  // Method without DisplayName falls back to hash key format.
  EXPECT_NE(out.find("<method>"), std::string::npos);
  // Field is still present.
  EXPECT_NE(out.find("value"), std::string::npos);
}

// ═════════════════════════════════════════════════════════════════════════════
// 17. Typed subclass support: instantiate<T>, addClass<T>, forEachChild<T>,
//     and to_field_value upcast for shared_ptr<T> where T derives from dynamic
// ═════════════════════════════════════════════════════════════════════════════

struct Widget : public dynamic {
  int tag = 0;
  explicit Widget(dynamic&& base) : dynamic(std::move(base)) {}
};

struct FooChild : public dynamic {
  explicit FooChild(dynamic&& b) : dynamic(std::move(b)) {}
};

struct BarChild : public dynamic {
  explicit BarChild(dynamic&& b) : dynamic(std::move(b)) {}
};

// ── clone_ptr(): virtual clone preserves subclass identity ──────────────────

// A dynamic subclass that opts into type-preserving cloning by overriding
// clone_ptr() and delegating the field-copy work to clone_into().
struct CloningWidget : public dynamic {
  int tag = 0;
  explicit CloningWidget(dynamic&& base) : dynamic(std::move(base)) {}

  dynamic_ptr clone_ptr() const override {
    dynamic_ptr result{std::make_shared<CloningWidget>(dynamic{})};
    clone_into(*result);
    static_cast<CloningWidget&>(*result).tag = tag;
    return result;
  }
};

TEST(DynamicTests, CloneWithoutOverrideSlicesNestedSubclass) {
  // FooChild does not override clone_ptr(), so the base implementation's
  // fallback (plain `dynamic`) applies — this documents today's default,
  // opt-in-only behavior rather than asserting it is desirable.
  dynamic parent;
  parent["child"_key] = dynamic_ptr(std::make_shared<FooChild>(dynamic::instantiate(bison_key_t{0U})));

  dynamic copy = parent.clone();
  auto cloned_child = copy["child"_key].as<dynamic_ptr>();
  ASSERT_NE(cloned_child, nullptr);
  EXPECT_EQ(dynamic_cast<FooChild*>(cloned_child.get()), nullptr);
}

TEST(DynamicTests, ClonePtrPreservesSubclassAtEveryDepth) {
  auto grandchild = std::make_shared<CloningWidget>(dynamic::instantiate(bison_key_t{0U}));
  grandchild->tag = 3;
  (*grandchild)["v"_key] = int32_t{30};

  auto child = std::make_shared<CloningWidget>(dynamic::instantiate(bison_key_t{0U}));
  child->tag = 2;
  (*child)["grandchild"_key] = dynamic_ptr{grandchild};

  auto root = std::make_shared<CloningWidget>(dynamic::instantiate(bison_key_t{0U}));
  root->tag = 1;
  (*root)["child"_key] = dynamic_ptr{child};

  dynamic_ptr cloned = root->clone_ptr();

  auto* cloned_root = dynamic_cast<CloningWidget*>(cloned.get());
  ASSERT_NE(cloned_root, nullptr);
  EXPECT_EQ(cloned_root->tag, 1);

  auto cloned_child_field = (*cloned_root)["child"_key].as<dynamic_ptr>();
  auto* cloned_child = dynamic_cast<CloningWidget*>(cloned_child_field.get());
  ASSERT_NE(cloned_child, nullptr);
  EXPECT_EQ(cloned_child->tag, 2);

  auto cloned_grandchild_field = (*cloned_child)["grandchild"_key].as<dynamic_ptr>();
  auto* cloned_grandchild = dynamic_cast<CloningWidget*>(cloned_grandchild_field.get());
  ASSERT_NE(cloned_grandchild, nullptr);
  EXPECT_EQ(cloned_grandchild->tag, 3);
  EXPECT_EQ((*cloned_grandchild)["v"_key].as<int32_t>(), 30);

  // Mutating the clone must not affect the original.
  (*cloned_grandchild)["v"_key] = int32_t{999};
  EXPECT_EQ((*grandchild)["v"_key].as<int32_t>(), 30);
}

// A dynamic subclass using the cloneable_dynamic<Derived> CRTP mixin instead
// of a hand-written clone_ptr() override — should behave identically to
// CloningWidget above without writing the override at all. Note this mixin
// only clones `dynamic` state (bison fields); a subclass with extra raw C++
// data members (like CloningWidget's `tag`) would still need to copy those
// itself, which is why this test keeps all identifying state in fields.
struct AutoCloningWidget : public cloneable_dynamic<AutoCloningWidget> {
  explicit AutoCloningWidget(dynamic&& base) : cloneable_dynamic(std::move(base)) {}
};

TEST(DynamicTests, CloneableDynamicMixinPreservesSubclassAtEveryDepth) {
  auto child = std::make_shared<AutoCloningWidget>(dynamic::instantiate(bison_key_t{0U}));
  (*child)["v"_key] = int32_t{20};

  auto root = std::make_shared<AutoCloningWidget>(dynamic::instantiate(bison_key_t{0U}));
  (*root)["child"_key] = dynamic_ptr{child};

  dynamic_ptr cloned = root->clone_ptr();

  auto* cloned_root = dynamic_cast<AutoCloningWidget*>(cloned.get());
  ASSERT_NE(cloned_root, nullptr);

  auto cloned_child_field = (*cloned_root)["child"_key].as<dynamic_ptr>();
  auto* cloned_child = dynamic_cast<AutoCloningWidget*>(cloned_child_field.get());
  ASSERT_NE(cloned_child, nullptr);
  EXPECT_EQ((*cloned_child)["v"_key].as<int32_t>(), 20);

  // Mutating the clone must not affect the original.
  (*cloned_child)["v"_key] = int32_t{999};
  EXPECT_EQ((*child)["v"_key].as<int32_t>(), 20);
}

class SubclassTest : public ::testing::Test {
 protected:
  void SetUp() override {
    clearClassRegistry();
  }
  void TearDown() override {
    clearClassRegistry();
  }
};

// ── to_field_value: shared_ptr<T> where T derives from dynamic ───────────────

TEST(SubclassFieldCoercionTests, SharedPtrSubclassStoredAsDynamicPtr) {
  auto w = std::make_shared<Widget>(dynamic::instantiate("Widget"_key));
  w->tag = 42;
  field f{w};
  EXPECT_TRUE(f.is<dynamic_ptr>());
  auto* wptr = dynamic_cast<Widget*>(f.as<dynamic_ptr>().get());
  ASSERT_NE(wptr, nullptr);
  EXPECT_EQ(wptr->tag, 42);
}

TEST(SubclassFieldCoercionTests, AssignSharedPtrSubclassToMonostateField) {
  auto w = std::make_shared<Widget>(dynamic::instantiate("Widget"_key));
  w->tag = 7;
  field f;
  f = w;
  EXPECT_TRUE(f.is<dynamic_ptr>());
  auto* wptr = dynamic_cast<Widget*>(f.as<dynamic_ptr>().get());
  ASSERT_NE(wptr, nullptr);
  EXPECT_EQ(wptr->tag, 7);
}

// ── instantiate<T> ───────────────────────────────────────────────────────────

TEST_F(SubclassTest, InstantiateTypedSubclassGlobalNamespace) {
  auto ptr = dynamic::instantiate<Widget>("Widget"_key);
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ((*ptr)["__class"_key].as<bison_key_t>().id, hash_t("Widget"_key));
}

TEST_F(SubclassTest, InstantiateTypedSubclassWithNamespace) {
  auto ptr = dynamic::instantiate<Widget>("ui"_key, "Widget"_key);
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ((*ptr)["__class"_key].as<bison_key_t>().id, hash_t("Widget"_key));
  EXPECT_EQ((*ptr)["__namespace"_key].as<bison_key_t>().id, hash("ui"));
}

TEST_F(SubclassTest, InstantiateTypedSubclassInheritsFields) {
  auto proto = std::make_shared<Widget>(dynamic::instantiate("WProto"_key));
  (*proto)["color"_key] = std::string{"red"};
  ASSERT_TRUE(dynamic::addClass("ui"_key, proto, bison_key_t{0U}));

  auto inst = dynamic::instantiate<Widget>("ui"_key, "WProto"_key);
  ASSERT_NE(inst, nullptr);
  EXPECT_EQ((*inst)["color"_key].as<std::string>(), "red");
}

// ── addClass<T>(ns, shared_ptr<T>, parent, class_attrs) ──────────────────────

TEST_F(SubclassTest, AddClassWithTypedSharedPtrSucceeds) {
  auto proto = std::make_shared<Widget>(dynamic::instantiate("WProto1"_key));
  (*proto)["size"_key] = int32_t{10};
  EXPECT_TRUE(dynamic::addClass("ui"_key, proto, bison_key_t{0U}));
}

TEST_F(SubclassTest, AddClassWithTypedSharedPtrDuplicateFails) {
  auto p1 = std::make_shared<Widget>(dynamic::instantiate("WProto2"_key));
  auto p2 = std::make_shared<Widget>(dynamic::instantiate("WProto2"_key));
  EXPECT_TRUE(dynamic::addClass("ui"_key, p1, bison_key_t{0U}));
  EXPECT_FALSE(dynamic::addClass("ui"_key, p2, bison_key_t{0U}));
}

TEST_F(SubclassTest, AddClassWithTypedSharedPtrAndClassAttrs) {
  auto proto = std::make_shared<Widget>(dynamic::instantiate("WProto3"_key));
  Widget* raw = proto.get();
  ASSERT_TRUE(
      dynamic::addClass("ui"_key, proto, bison_key_t{0U}, {attr<DisplayName>("My Widget"), attr<Description>("hint")}));

  // proto was moved; raw still valid via the registry's shared ownership.
  auto* dn = (*raw)[dynamic::CLASS].findAttribute<DisplayName>();
  ASSERT_NE(dn, nullptr);
  EXPECT_EQ(dn->name(), "My Widget");
  auto* desc = (*raw)[dynamic::CLASS].findAttribute<Description>();
  ASSERT_NE(desc, nullptr);
  EXPECT_EQ(desc->text(), "hint");
}

TEST_F(SubclassTest, AddClassWithTypedSharedPtrFieldsInheritable) {
  auto base = std::make_shared<Widget>(dynamic::instantiate("WBase"_key));
  (*base)["visible"_key] = true;
  ASSERT_TRUE(dynamic::addClass("ui"_key, base, bison_key_t{0U}));

  auto child = std::make_shared<Widget>(dynamic::instantiate("WChild"_key));
  ASSERT_TRUE(dynamic::addClass("ui"_key, child, "WBase"_key));

  auto inst = dynamic::instantiate<Widget>("ui"_key, "WChild"_key);
  ASSERT_NE(inst, nullptr);
  EXPECT_TRUE((*inst)["visible"_key].as<bool>());
}

// ── forEachChild<T> ──────────────────────────────────────────────────────────

TEST(ForEachChildTests, VisitsOnlyMatchingSubtype) {
  dynamic parent;
  parent["a"_key] = dynamic_ptr(std::make_shared<FooChild>(dynamic::instantiate(bison_key_t{0U})));
  parent["b"_key] = dynamic_ptr(std::make_shared<BarChild>(dynamic::instantiate(bison_key_t{0U})));
  parent["c"_key] = dynamic_ptr(std::make_shared<FooChild>(dynamic::instantiate(bison_key_t{0U})));

  int count = 0;
  parent.forEachChild<FooChild>([&count](bison_key_t, FooChild&) { ++count; });
  EXPECT_EQ(count, 2);
}

TEST(ForEachChildTests, SkipsNonDynamicPtrFields) {
  dynamic parent;
  parent["x"_key] = int32_t{1};
  parent["y"_key] = std::string{"hello"};

  int count = 0;
  parent.forEachChild<FooChild>([&count](bison_key_t, FooChild&) { ++count; });
  EXPECT_EQ(count, 0);
}

TEST(ForEachChildTests, SkipsNullDynamicPtr) {
  dynamic parent;
  parent["p"_key] = std::shared_ptr<dynamic>{};

  int count = 0;
  parent.forEachChild<FooChild>([&count](bison_key_t, FooChild&) { ++count; });
  EXPECT_EQ(count, 0);
}

TEST(ForEachChildTests, VisitorReceivesCorrectKeyAndRef) {
  dynamic parent;
  auto child = std::make_shared<FooChild>(dynamic::instantiate(bison_key_t{0U}));
  (*child)["val"_key] = int32_t{99};
  parent["child"_key] = dynamic_ptr(child);

  bison_key_t visited_key{0U};
  int visited_val = 0;
  parent.forEachChild<FooChild>([&](bison_key_t k, FooChild& f) {
    visited_key = k;
    visited_val = f["val"_key].as<int32_t>();
  });

  EXPECT_EQ(visited_key.id, hash_t("child"_key));
  EXPECT_EQ(visited_val, 99);
}

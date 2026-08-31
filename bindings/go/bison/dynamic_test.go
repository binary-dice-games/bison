// MIT License © 2025 Binary Dice Games

// Tests for the bison package's Dynamic wrapper. Mirrors
// bindings/python/tests/test_dynamic.py's / bindings/rust/tests/
// dynamic_tests.rs's coverage. Run with: go test ./...
package bison

import (
	"math"
	"strings"
	"sync"
	"testing"
)

// clear_registry()/AddClass() touch a process-wide C registry; go test runs
// tests within one package sequentially by default (only t.Parallel() tests
// run concurrently, and this file never calls it), but keep a lock anyway
// as cheap insurance against a future change to that default.
var registryLock sync.Mutex

func must(t *testing.T, err error) {
	t.Helper()
	if err != nil {
		t.Fatal(err)
	}
}

func assertBisonCode(t *testing.T, err error, want int32) {
	t.Helper()
	if err == nil {
		t.Fatal("expected an error")
	}
	be, ok := err.(*BisonError)
	if !ok {
		t.Fatalf("expected *BisonError, got %T: %v", err, err)
	}
	if be.Code != want {
		t.Fatalf("code = %d, want %d", be.Code, want)
	}
}

// ═════════════════════════════════════════════════════════════════════════
// Lifecycle
// ═════════════════════════════════════════════════════════════════════════

func TestCreateSucceeds(t *testing.T) {
	obj, err := New("")
	must(t, err)
	defer obj.Close()
}

func TestAddRefSharesMutations(t *testing.T) {
	obj, _ := New("")
	defer obj.Close()
	must(t, obj.SetInt("n", 10))

	ref, err := obj.AddRef()
	must(t, err)
	defer ref.Close()

	must(t, obj.SetInt("n", 20))
	n, err := ref.GetInt("n")
	must(t, err)
	if n != 20 {
		t.Fatalf("expected 20, got %d", n)
	}

	must(t, ref.SetInt("n", 30))
	n, err = obj.GetInt("n")
	must(t, err)
	if n != 30 {
		t.Fatalf("expected 30, got %d", n)
	}
}

func TestCloneIsIndependent(t *testing.T) {
	obj, _ := New("")
	defer obj.Close()
	must(t, obj.SetInt("n", 1))

	clone, err := obj.Clone()
	must(t, err)
	defer clone.Close()
	must(t, clone.SetInt("n", 2))

	n1, err := obj.GetInt("n")
	must(t, err)
	if n1 != 1 {
		t.Fatal("original mutated by clone")
	}
	n2, err := clone.GetInt("n")
	must(t, err)
	if n2 != 2 {
		t.Fatal("clone did not take its own value")
	}
}

func TestCloseIsIdempotent(t *testing.T) {
	obj, _ := New("")
	must(t, obj.Close())
	must(t, obj.Close())
}

// ═════════════════════════════════════════════════════════════════════════
// Field access
// ═════════════════════════════════════════════════════════════════════════

func TestScalarRoundTrip(t *testing.T) {
	obj, _ := New("")
	defer obj.Close()
	must(t, obj.SetInt("score", 42))
	must(t, obj.SetFloat("speed", 9.5))
	must(t, obj.SetBool("alive", true))
	must(t, obj.SetString("name", "hero"))

	score, err := obj.GetInt("score")
	must(t, err)
	if score != 42 {
		t.Fatalf("score = %d", score)
	}
	speed, err := obj.GetFloat("speed")
	must(t, err)
	if math.Abs(float64(speed-9.5)) > 1e-6 {
		t.Fatalf("speed = %v", speed)
	}
	alive, err := obj.GetBool("alive")
	must(t, err)
	if !alive {
		t.Fatal("alive = false")
	}
	name, err := obj.GetString("name")
	must(t, err)
	if name != "hero" {
		t.Fatalf("name = %q", name)
	}
}

func TestTypeLockedFieldErrors(t *testing.T) {
	obj, _ := New("")
	defer obj.Close()
	must(t, obj.SetInt("score", 1))
	err := obj.SetFloat("score", 1.5)
	assertBisonCode(t, err, ErrType)
}

func TestNestedObject(t *testing.T) {
	obj, _ := New("")
	defer obj.Close()
	child, _ := New("")
	defer child.Close()
	must(t, child.SetString("city", "Springfield"))
	must(t, obj.SetObject("address", child))

	addr, err := obj.GetObject("address")
	must(t, err)
	defer addr.Close()
	city, err := addr.GetString("city")
	must(t, err)
	if city != "Springfield" {
		t.Fatalf("city = %q", city)
	}
}

func TestNullObjectRoundTrip(t *testing.T) {
	obj, _ := New("")
	defer obj.Close()
	must(t, obj.SetObject("address", nil))
	addr, err := obj.GetObject("address")
	must(t, err)
	if addr != nil {
		t.Fatalf("expected nil, got %v", addr)
	}
}

func TestIndexedFieldsAndSize(t *testing.T) {
	obj, _ := New("")
	defer obj.Close()
	must(t, obj.SetAt(0, "red"))
	must(t, obj.SetAt(1, "green"))
	must(t, obj.SetAt(2, "blue"))

	if obj.Size() != 3 {
		t.Fatalf("size = %d", obj.Size())
	}
	v1, err := obj.GetStringAt(1)
	must(t, err)
	if v1 != "green" {
		t.Fatalf("[1] = %q", v1)
	}

	var got []string
	err = obj.Each(func(_ int, v interface{}) error {
		s, ok := v.(string)
		if !ok {
			t.Fatalf("expected string, got %T", v)
		}
		got = append(got, s)
		return nil
	})
	must(t, err)
	want := []string{"red", "green", "blue"}
	for i, w := range want {
		if got[i] != w {
			t.Fatalf("got[%d] = %q, want %q", i, got[i], w)
		}
	}
}

func TestIndexedTypeLock(t *testing.T) {
	obj, _ := New("")
	defer obj.Close()
	must(t, obj.SetAt(0, int32(10)))
	if err := obj.SetAt(0, float32(1.5)); err == nil {
		t.Fatal("expected type-mismatch error")
	}
}

func TestIndexedBoolRoundTripsAsBool(t *testing.T) {
	obj, _ := New("")
	defer obj.Close()
	must(t, obj.SetAt(0, true))
	must(t, obj.SetAt(1, false))

	v0, err := obj.GetAt(0)
	must(t, err)
	if b, ok := v0.(bool); !ok || !b {
		t.Fatalf("[0] = %#v", v0)
	}
	v1, err := obj.GetAt(1)
	must(t, err)
	if b, ok := v1.(bool); !ok || b {
		t.Fatalf("[1] = %#v", v1)
	}
}

func TestIndexedObjectRoundTrip(t *testing.T) {
	obj, _ := New("")
	defer obj.Close()
	child, _ := New("")
	defer child.Close()
	must(t, child.SetInt("v", 9))
	must(t, obj.SetObjectAt(0, child))

	out, err := obj.GetObjectAt(0)
	must(t, err)
	defer out.Close()
	v, err := out.GetInt("v")
	must(t, err)
	if v != 9 {
		t.Fatalf("v = %d", v)
	}
}

func TestSetKeyAtRoundTrips(t *testing.T) {
	obj, _ := New("")
	defer obj.Close()
	must(t, obj.SetKeyAt(0, Key("hero")))
	v, err := obj.GetKeyAt(0)
	must(t, err)
	if v != Key("hero") {
		t.Fatalf("key = %v", v)
	}
}

// ═════════════════════════════════════════════════════════════════════════
// key_t-typed field access
// ═════════════════════════════════════════════════════════════════════════

func TestKeyRoundTripsWithHashedValue(t *testing.T) {
	obj, _ := New("")
	defer obj.Close()
	must(t, obj.SetKey("id", Key("hero")))
	v, err := obj.GetKey("id")
	must(t, err)
	if v != Key("hero") {
		t.Fatalf("id = %v", v)
	}
}

func TestKeyAcceptsStringValue(t *testing.T) {
	obj, _ := New("")
	defer obj.Close()
	must(t, obj.SetKey("id", "hero"))
	v, err := obj.GetKey("id")
	must(t, err)
	if v != Key("hero") {
		t.Fatalf("id = %v", v)
	}
}

func TestKeyDistinctFromInt32Field(t *testing.T) {
	obj, _ := New("")
	defer obj.Close()
	must(t, obj.SetInt("plain_int", 42))
	err := obj.SetKey("plain_int", Key("anything"))
	assertBisonCode(t, err, ErrType)
}

func TestGetFallsBackToKeyAfterOtherTypesFail(t *testing.T) {
	obj, _ := New("")
	defer obj.Close()
	must(t, obj.SetKey("selector", Key("nav_mode_topdown")))
	v, err := obj.Get("selector")
	must(t, err)
	k, ok := v.(uint32)
	if !ok || k != Key("nav_mode_topdown") {
		t.Fatalf("Get(selector) = %#v", v)
	}
}

func TestAddFieldKeyDeclaresAndRejectsDuplicate(t *testing.T) {
	obj, _ := New("")
	defer obj.Close()
	must(t, obj.AddFieldKey("id", Key("hero"), nil))
	v, err := obj.GetKey("id")
	must(t, err)
	if v != Key("hero") {
		t.Fatalf("id = %v", v)
	}
	err = obj.AddFieldKey("id", Key("other"), nil)
	assertBisonCode(t, err, ErrDuplicate)
}

// ═════════════════════════════════════════════════════════════════════════
// Vector fields
// ═════════════════════════════════════════════════════════════════════════

func assertInt32Slice(t *testing.T, got, want []int32) {
	t.Helper()
	if len(got) != len(want) {
		t.Fatalf("got %v want %v", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("got %v want %v", got, want)
		}
	}
}

func TestVectorIntRoundTrip(t *testing.T) {
	obj, _ := New("")
	defer obj.Close()
	must(t, obj.SetVectorInt("ints", []int32{1, 2, 3}))
	got, err := obj.GetVectorInt("ints")
	must(t, err)
	assertInt32Slice(t, got, []int32{1, 2, 3})
}

func TestVectorBoolRoundTrip(t *testing.T) {
	obj, _ := New("")
	defer obj.Close()
	must(t, obj.SetVectorBool("flags", []bool{true, false, true}))
	got, err := obj.GetVectorBool("flags")
	must(t, err)
	want := []bool{true, false, true}
	if len(got) != len(want) {
		t.Fatalf("got %v want %v", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("got %v want %v", got, want)
		}
	}
}

func TestVectorFloatRoundTrip(t *testing.T) {
	obj, _ := New("")
	defer obj.Close()
	must(t, obj.SetVectorFloat("ratios", []float32{1.5, 2.5}))
	got, err := obj.GetVectorFloat("ratios")
	must(t, err)
	if len(got) != 2 || got[0] != 1.5 || got[1] != 2.5 {
		t.Fatalf("got %v", got)
	}
}

func TestVectorBytesRoundTrip(t *testing.T) {
	obj, _ := New("")
	defer obj.Close()
	must(t, obj.SetVectorBytes("blob", []byte{0, 1, 255}))
	got, err := obj.GetVectorBytes("blob")
	must(t, err)
	if len(got) != 3 || got[0] != 0 || got[1] != 1 || got[2] != 255 {
		t.Fatalf("got %v", got)
	}
}

func TestVectorAssignmentReplacesExistingContents(t *testing.T) {
	obj, _ := New("")
	defer obj.Close()
	must(t, obj.SetVectorInt("ints", []int32{1, 2, 3}))
	must(t, obj.SetVectorInt("ints", []int32{9, 9}))
	got, err := obj.GetVectorInt("ints")
	must(t, err)
	assertInt32Slice(t, got, []int32{9, 9})
}

func TestVectorEmptyRoundTrips(t *testing.T) {
	obj, _ := New("")
	defer obj.Close()
	must(t, obj.SetVectorInt("empty", nil))
	got, err := obj.GetVectorInt("empty")
	must(t, err)
	if len(got) != 0 {
		t.Fatalf("got %v", got)
	}
}

func TestAddFieldVectorRegistersAndIsReadable(t *testing.T) {
	obj, _ := New("")
	defer obj.Close()
	must(t, obj.AddField("ints", []int32{1, 2, 3}, nil))
	got, err := obj.GetVectorInt("ints")
	must(t, err)
	assertInt32Slice(t, got, []int32{1, 2, 3})
}

func TestAddFieldVectorRejectsDuplicate(t *testing.T) {
	obj, _ := New("")
	defer obj.Close()
	must(t, obj.AddField("ints", []int32{1, 2, 3}, nil))
	err := obj.AddField("ints", []int32{9}, nil)
	assertBisonCode(t, err, ErrDuplicate)
}

func TestAddFieldBytes(t *testing.T) {
	obj, _ := New("")
	defer obj.Close()
	must(t, obj.AddField("blob", []byte{1, 2, 3}, nil))
	got, err := obj.GetVectorBytes("blob")
	must(t, err)
	if len(got) != 3 {
		t.Fatalf("got %v", got)
	}
}

// ═════════════════════════════════════════════════════════════════════════
// Generic Get/Set
// ═════════════════════════════════════════════════════════════════════════

func TestGenericSetAcceptsPlainInt(t *testing.T) {
	obj, _ := New("")
	defer obj.Close()
	must(t, obj.Set("age", 30))
	age, err := obj.GetInt("age")
	must(t, err)
	if age != 30 {
		t.Fatalf("age = %d", age)
	}
}

func TestGenericGetCascade(t *testing.T) {
	obj, _ := New("")
	defer obj.Close()
	must(t, obj.Set("n", int32(5)))
	must(t, obj.Set("f", float32(1.5)))
	must(t, obj.Set("b", true))
	must(t, obj.Set("s", "hi"))

	if v, err := obj.Get("n"); err != nil || v.(int32) != 5 {
		t.Fatalf("n: %v, %v", v, err)
	}
	if v, err := obj.Get("f"); err != nil || v.(float32) != 1.5 {
		t.Fatalf("f: %v, %v", v, err)
	}
	if v, err := obj.Get("b"); err != nil || v.(bool) != true {
		t.Fatalf("b: %v, %v", v, err)
	}
	if v, err := obj.Get("s"); err != nil || v.(string) != "hi" {
		t.Fatalf("s: %v, %v", v, err)
	}
}

// ═════════════════════════════════════════════════════════════════════════
// Methods
// ═════════════════════════════════════════════════════════════════════════

func TestAddMethodAndCall(t *testing.T) {
	calc, _ := New("")
	defer calc.Close()
	err := calc.AddMethod("add", func(self, params, result *Dynamic) {
		a, _ := params.GetInt("a")
		b, _ := params.GetInt("b")
		_ = result.SetInt("value", a+b)
	})
	must(t, err)

	args, _ := New("")
	defer args.Close()
	must(t, args.SetInt("a", 10))
	must(t, args.SetInt("b", 32))

	out, err := calc.Call("add", args)
	must(t, err)
	defer out.Close()
	v, err := out.GetInt("value")
	must(t, err)
	if v != 42 {
		t.Fatalf("value = %d", v)
	}
}

func TestCallUnknownMethodErrors(t *testing.T) {
	obj, _ := New("")
	defer obj.Close()
	_, err := obj.Call("nope", nil)
	assertBisonCode(t, err, ErrNotFound)
}

func TestAddMethodPanicIsRecovered(t *testing.T) {
	obj, _ := New("")
	defer obj.Close()
	err := obj.AddMethod("boom", func(self, params, result *Dynamic) {
		panic("should not escape")
	})
	must(t, err)
	// Must not crash the test process.
	_, _ = obj.Call("boom", nil)
}

// ═════════════════════════════════════════════════════════════════════════
// Class registry / inheritance
// ═════════════════════════════════════════════════════════════════════════

func TestClassInheritance(t *testing.T) {
	registryLock.Lock()
	defer registryLock.Unlock()
	ClearRegistry()
	defer ClearRegistry()

	shape, _ := New("Shape_go_inherit")
	must(t, shape.SetString("color", "black"))
	must(t, AddClass(shape, "", "", nil))

	circle, _ := New("Circle_go_inherit")
	must(t, circle.SetFloat("radius", 1.0))
	must(t, AddClass(circle, "Shape_go_inherit", "", nil))

	c, err := Instantiate("Circle_go_inherit", "")
	must(t, err)
	defer c.Close()
	color, err := c.GetString("color")
	must(t, err)
	if color != "black" {
		t.Fatalf("color = %q", color)
	}
	radius, err := c.GetFloat("radius")
	must(t, err)
	if math.Abs(float64(radius-1.0)) > 1e-6 {
		t.Fatalf("radius = %v", radius)
	}
}

func TestDuplicateClassErrors(t *testing.T) {
	registryLock.Lock()
	defer registryLock.Unlock()
	ClearRegistry()
	defer ClearRegistry()

	proto, _ := New("Shape_go_dup")
	must(t, AddClass(proto, "", "", nil))
	dup, _ := New("Shape_go_dup")
	err := AddClass(dup, "", "", nil)
	assertBisonCode(t, err, ErrDuplicate)
	_ = dup.Close()
}

func TestFindClassLookup(t *testing.T) {
	registryLock.Lock()
	defer registryLock.Unlock()
	ClearRegistry()
	defer ClearRegistry()

	proto, _ := New("Shape_go_find")
	must(t, AddClass(proto, "", "", nil))
	found, err := FindClass("Shape_go_find", "")
	must(t, err)
	if found == nil {
		t.Fatal("expected to find Shape_go_find")
	}
	missing, err := FindClass("DoesNotExist_go", "")
	must(t, err)
	if missing != nil {
		t.Fatal("expected nil for missing class")
	}
}

func TestNamespacesIsolateSameName(t *testing.T) {
	registryLock.Lock()
	defer registryLock.Unlock()
	ClearRegistry()
	defer ClearRegistry()

	mathTable, _ := New("table_go")
	must(t, mathTable.SetInt("rows", 1))
	must(t, AddClass(mathTable, "", "math_go", nil))

	ikeaTable, _ := New("table_go")
	must(t, ikeaTable.SetInt("legs", 4))
	must(t, AddClass(ikeaTable, "", "ikea_go", nil))

	mt, err := Instantiate("table_go", "math_go")
	must(t, err)
	defer mt.Close()
	it, err := Instantiate("table_go", "ikea_go")
	must(t, err)
	defer it.Close()

	rows, err := mt.GetInt("rows")
	must(t, err)
	if rows != 1 {
		t.Fatalf("rows = %d", rows)
	}
	legs, err := it.GetInt("legs")
	must(t, err)
	if legs != 4 {
		t.Fatalf("legs = %d", legs)
	}
}

func TestClassAndFieldAttributes(t *testing.T) {
	registryLock.Lock()
	defer registryLock.Unlock()
	ClearRegistry()
	defer ClearRegistry()

	proto, _ := New("Widget_go")
	must(t, proto.AddField("count", int32(0), &Attributes{
		Description: "a counter",
		Required:    true,
	}))
	must(t, AddClass(proto, "", "", &Attributes{DisplayName: "Widget class"}))

	attrs, err := ClassAttributes("Widget_go", "")
	must(t, err)
	if attrs.DisplayName != "Widget class" {
		t.Fatalf("display_name = %q", attrs.DisplayName)
	}

	w, err := Instantiate("Widget_go", "")
	must(t, err)
	defer w.Close()
	fieldAttrs, err := w.FieldAttributes("count")
	must(t, err)
	if fieldAttrs.Description != "a counter" || !fieldAttrs.Required {
		t.Fatalf("field attrs = %#v", fieldAttrs)
	}
}

func TestRegisteredMethodSurvivesPrototypeGoingOutOfScope(t *testing.T) {
	registryLock.Lock()
	defer registryLock.Unlock()
	ClearRegistry()
	defer ClearRegistry()

	func() {
		proto, _ := New("Doubler_go")
		err := proto.AddMethod("double", func(self, params, result *Dynamic) {
			n, _ := params.GetInt("n")
			_ = result.SetInt("value", n*2)
		})
		must(t, err)
		must(t, AddClass(proto, "", "", nil))
		// proto goes out of scope here, but AddClass kept a package-level
		// reference to it (and hence to the AddMethod trampoline).
	}()

	inst, err := Instantiate("Doubler_go", "")
	must(t, err)
	defer inst.Close()
	args, _ := New("")
	defer args.Close()
	must(t, args.SetInt("n", 21))
	out, err := inst.Call("double", args)
	must(t, err)
	defer out.Close()
	v, err := out.GetInt("value")
	must(t, err)
	if v != 42 {
		t.Fatalf("value = %d", v)
	}
}

// ═════════════════════════════════════════════════════════════════════════
// JSON / YAML import
// ═════════════════════════════════════════════════════════════════════════

func TestFromJSONParses(t *testing.T) {
	obj, err := FromJSON(`{"x": 1, "y": 2.5, "tags": ["a", "b"]}`)
	must(t, err)
	defer obj.Close()
	x, err := obj.GetInt("x")
	must(t, err)
	if x != 1 {
		t.Fatalf("x = %d", x)
	}
	y, err := obj.GetFloat("y")
	must(t, err)
	if math.Abs(float64(y-2.5)) > 1e-6 {
		t.Fatalf("y = %v", y)
	}
	tags, err := obj.GetObject("tags")
	must(t, err)
	defer tags.Close()
	tag0, err := tags.GetStringAt(0)
	must(t, err)
	if tag0 != "a" {
		t.Fatalf("tags[0] = %q", tag0)
	}
	tag1, err := tags.GetStringAt(1)
	must(t, err)
	if tag1 != "b" {
		t.Fatalf("tags[1] = %q", tag1)
	}
}

func TestFromYAMLParses(t *testing.T) {
	obj, err := FromYAML("x: 10\nname: test\n")
	must(t, err)
	defer obj.Close()
	x, err := obj.GetInt("x")
	must(t, err)
	if x != 10 {
		t.Fatalf("x = %d", x)
	}
	name, err := obj.GetString("name")
	must(t, err)
	if name != "test" {
		t.Fatalf("name = %q", name)
	}
}

func TestToJSONProducesValidJSON(t *testing.T) {
	obj, err := FromJSON(`{"x": 1}`)
	must(t, err)
	defer obj.Close()
	s, err := obj.ToJSON(-1)
	must(t, err)
	if !strings.Contains(s, "1") {
		t.Fatalf("json = %q", s)
	}
}

func TestInvalidJSONErrors(t *testing.T) {
	_, err := FromJSON("not json")
	if err == nil {
		t.Fatal("expected error")
	}
}

// ═════════════════════════════════════════════════════════════════════════
// Binary serialization
// ═════════════════════════════════════════════════════════════════════════

func TestBinaryRoundTripsScalarFields(t *testing.T) {
	obj, _ := New("")
	defer obj.Close()
	must(t, obj.SetInt("x", 42))
	must(t, obj.SetFloat("y", 2.5))
	must(t, obj.SetString("s", "hello"))

	buf, err := obj.Serialize()
	must(t, err)
	if len(buf) == 0 {
		t.Fatal("empty buffer")
	}

	decoded, err := Deserialize(buf)
	must(t, err)
	defer decoded.Close()
	x, err := decoded.GetInt("x")
	must(t, err)
	if x != 42 {
		t.Fatalf("x = %d", x)
	}
	y, err := decoded.GetFloat("y")
	must(t, err)
	if math.Abs(float64(y-2.5)) > 1e-6 {
		t.Fatalf("y = %v", y)
	}
	s, err := decoded.GetString("s")
	must(t, err)
	if s != "hello" {
		t.Fatalf("s = %q", s)
	}
}

func TestBinaryRoundTripsNestedObject(t *testing.T) {
	obj, _ := New("")
	defer obj.Close()
	child, _ := New("")
	defer child.Close()
	must(t, child.SetString("city", "Springfield"))
	must(t, obj.SetObject("address", child))

	buf, err := obj.Serialize()
	must(t, err)
	decoded, err := Deserialize(buf)
	must(t, err)
	defer decoded.Close()
	addr, err := decoded.GetObject("address")
	must(t, err)
	defer addr.Close()
	city, err := addr.GetString("city")
	must(t, err)
	if city != "Springfield" {
		t.Fatalf("city = %q", city)
	}
}

func TestBinaryMalformedBufferErrors(t *testing.T) {
	_, err := Deserialize([]byte{0xff, 0x00, 0x01})
	assertBisonCode(t, err, ErrParse)
}

func TestBinaryEmptyObjectRoundTrips(t *testing.T) {
	obj, _ := New("")
	defer obj.Close()
	buf, err := obj.Serialize()
	must(t, err)
	decoded, err := Deserialize(buf)
	must(t, err)
	defer decoded.Close()
	if decoded.Size() != 0 {
		t.Fatalf("size = %d", decoded.Size())
	}
}

// ═════════════════════════════════════════════════════════════════════════
// Key hashing
// ═════════════════════════════════════════════════════════════════════════

func TestKeyIsStable(t *testing.T) {
	if Key("velocity") != Key("velocity") {
		t.Fatal("Key is not stable")
	}
}

func TestKeyHighBitSet(t *testing.T) {
	if Key("velocity")&0x80000000 == 0 {
		t.Fatal("high bit not set")
	}
}

func TestKeyDifferentNamesDiffer(t *testing.T) {
	if Key("velocity") == Key("score") {
		t.Fatal("distinct names hashed to the same key")
	}
}

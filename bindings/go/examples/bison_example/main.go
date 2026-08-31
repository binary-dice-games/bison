// MIT License © 2025 Binary Dice Games

// Detailed, runnable examples for the Bison dynamic-object Go binding.
// Mirrors bindings/python/examples/bison_example.py /
// bindings/rust/examples/bison_example.rs feature-for-feature.
//
// Run with: go run ./examples/bison_example
package main

import (
	"fmt"
	"math"

	"github.com/binary-dice-games/bison/bindings/go/bison"
)

var sectionIndex int

func section(title string) {
	sectionIndex++
	fmt.Println()
	fmt.Println("==========================================")
	fmt.Printf("  %d. %s\n", sectionIndex, title)
	fmt.Println("==========================================")
}

func check(err error) {
	if err != nil {
		panic(err)
	}
}

// ── Example 1: Hashing and keys ─────────────────────────────────────────────

func exampleHashing() {
	section("Hashing and keys")
	k1 := bison.Key("velocity")
	k2 := bison.Key("velocity")
	fmt.Printf("Key(\"velocity\") is stable: %v\n", k1 == k2)
	fmt.Printf("High bit set on named key: %v\n", (k1&0x80000000) != 0)
	fmt.Printf("\"velocity\" != \"score\": %v\n", k1 != bison.Key("score"))
}

// ── Example 2: Scalar field get / set ───────────────────────────────────────

func exampleScalarFields() {
	section("Scalar field get / set")
	h, err := bison.New("Person")
	check(err)
	defer h.Close()
	check(h.SetString("name", "Alice"))
	check(h.SetInt("age", 30))
	check(h.SetFloat("score", 9.5))
	check(h.SetBool("active", true))

	name, _ := h.GetString("name")
	age, _ := h.GetInt("age")
	score, _ := h.GetFloat("score")
	active, _ := h.GetBool("active")
	fmt.Printf("name   : %s\n", name)
	fmt.Printf("age    : %d\n", age)
	fmt.Printf("score  : %v\n", score)
	fmt.Printf("active : %v\n", active)
}

// ── Example 3: Nested objects ───────────────────────────────────────────────

func exampleNestedObjects() {
	section("Nested objects")
	person, _ := bison.New("Person")
	defer person.Close()
	check(person.SetString("name", "Alice"))

	address, _ := bison.NewAnonymous()
	defer address.Close()
	check(address.SetString("street", "123 Main St"))
	check(address.SetString("city", "Springfield"))

	// SetObject increments address's ref-count; the local `address` value
	// still owns its own independent handle and is closed normally above.
	check(person.SetObject("address", address))

	addrOut, err := person.GetObject("address")
	check(err)
	defer addrOut.Close()
	city, _ := addrOut.GetString("city")
	fmt.Printf("city: %s\n", city)
}

// ── Example 4: Numeric (array-like) indexing ────────────────────────────────

func exampleNumericIndexing() {
	section("Numeric (array-like) indexing")
	lst, _ := bison.NewAnonymous()
	defer lst.Close()
	check(lst.SetAt(0, "red"))
	check(lst.SetAt(1, "green"))
	check(lst.SetAt(2, "blue"))
	fmt.Printf("list size : %d\n", lst.Size())
	v1, _ := lst.GetStringAt(1)
	fmt.Printf("list[1]   : %s\n", v1)

	scores, _ := bison.NewAnonymous()
	defer scores.Close()
	check(scores.SetAt(0, int32(10)))
	check(scores.SetAt(1, int32(20)))
	check(scores.SetAt(2, int32(30)))
	v2, _ := scores.GetIntAt(2)
	fmt.Printf("scores[2] : %d\n", v2)

	if err := scores.SetAt(0, float32(1.1)); err == nil {
		fmt.Println("type mismatch at[0]: unexpected (no error raised)")
	} else {
		fmt.Printf("type mismatch at[0]: BISON_ERR_TYPE (expected) -> %v\n", err)
	}

	fscores, _ := bison.NewAnonymous()
	defer fscores.Close()
	check(fscores.SetAt(0, float32(1.1)))
	check(fscores.SetAt(1, float32(2.2)))
	v3, _ := fscores.GetFloatAt(0)
	fmt.Printf("fscores[0]: %v\n", v3)
}

// ── Example 5: Methods — attaching behaviour to objects ─────────────────────

func exampleMethods() {
	section("Methods - attaching behaviour to objects")
	calc, _ := bison.New("Calculator")
	defer calc.Close()
	check(calc.SetInt("total", 0))
	check(calc.AddMethod("add", func(self, params, result *bison.Dynamic) {
		a, _ := params.GetInt("a")
		b, _ := params.GetInt("b")
		_ = result.SetInt("value", a+b)
	}))
	check(calc.AddMethod("accumulate", func(self, params, result *bison.Dynamic) {
		total, _ := self.GetInt("total")
		n, _ := params.GetInt("n")
		_ = result.SetInt("total", total+n)
	}))

	args, _ := bison.NewAnonymous()
	defer args.Close()
	check(args.SetInt("a", 10))
	check(args.SetInt("b", 32))
	total, err := calc.Call("add", args)
	check(err)
	v, _ := total.GetInt("value")
	fmt.Printf("10 + 32 = %d\n", v)
	total.Close()

	// `accumulate` above reads self.total but the callback receives a
	// non-owning view of self, so mutating self.total there wouldn't
	// persist -- mirrors the other bindings' method-callback semantics
	// (the callback populates `result` in place). Drive the running total
	// from the caller side instead.
	running := int32(0)
	for i := int32(1); i <= 5; i++ {
		running += i
		check(calc.SetInt("total", running))
		p, _ := bison.NewAnonymous()
		check(p.SetInt("n", i))
		_, err := calc.Call("accumulate", p)
		check(err)
		p.Close()
	}
	finalTotal, _ := calc.GetInt("total")
	fmt.Printf("accumulated total (1+2+3+4+5): %d\n", finalTotal)

	if _, err := calc.MethodAttributes("sqrt"); err == nil {
		fmt.Println("attribute access to unknown method: unexpected (no error raised)")
	} else {
		fmt.Printf("attribute access to unknown method: not found (expected) -> %v\n", err)
	}

	if _, err := calc.Call("sqrt", nil); err == nil {
		fmt.Println("call unknown method: unexpected (no error raised)")
	} else {
		fmt.Printf("call unknown method: BISON_ERR_NOT_FOUND (expected) -> %v\n", err)
	}
}

// ── Example 6: Class hierarchy and inheritance ──────────────────────────────

func exampleInheritance() {
	section("Class hierarchy and inheritance")
	bison.ClearRegistry()

	shape, _ := bison.New("Shape")
	check(shape.SetString("color", "black"))
	check(shape.AddMethod("describe", func(self, params, result *bison.Dynamic) {
		color, _ := self.GetString("color")
		_ = result.SetString("text", color+" shape")
	}))
	check(bison.AddClass(shape, "", "", nil))

	circle, _ := bison.New("Circle")
	check(circle.SetFloat("radius", 1.0))
	check(circle.AddMethod("area", func(self, params, result *bison.Dynamic) {
		r, _ := self.GetFloat("radius")
		_ = result.SetFloat("area", math.Pi*r*r)
	}))
	check(bison.AddClass(circle, "Shape", "", nil))

	c, err := bison.Instantiate("Circle", "")
	check(err)
	defer c.Close()
	check(c.SetFloat("radius", 5.0))
	check(c.SetString("color", "red")) // overrides the inherited default

	areaResult, err := c.Call("area", nil)
	check(err)
	area, _ := areaResult.GetFloat("area")
	fmt.Printf("Circle area (r=5): %.4f\n", area)
	areaResult.Close()

	descResult, err := c.Call("describe", nil)
	check(err)
	text, _ := descResult.GetString("text")
	fmt.Printf("Description: %s\n", text)
	descResult.Close()

	c2, err := bison.Instantiate("Circle", "")
	check(err)
	color2, _ := c2.GetString("color")
	fmt.Printf("Inherited color: %s\n", color2)
	c2.Close()

	dup, _ := bison.New("Shape")
	if err := bison.AddClass(dup, "", "", nil); err == nil {
		fmt.Println("Duplicate addClass rejected: false (unexpected)")
	} else {
		fmt.Println("Duplicate addClass rejected: true")
	}
	dup.Close()

	found, _ := bison.FindClass("Shape", "")
	fmt.Printf("Shape found in registry: %v\n", found != nil) // non-owning; do not Close

	bison.ClearRegistry()
}

// ── Example 7: Namespaces ───────────────────────────────────────────────────

func exampleNamespaces() {
	section("Namespaces - class isolation by unit")
	bison.ClearRegistry()

	mathTable, _ := bison.New("table")
	check(mathTable.SetInt("rows", 0))
	check(mathTable.SetInt("cols", 0))
	check(bison.AddClass(mathTable, "", "math", nil))

	ikeaTable, _ := bison.New("table")
	check(ikeaTable.SetInt("legs", 4))
	check(ikeaTable.SetString("material", "wood"))
	check(bison.AddClass(ikeaTable, "", "ikea", nil))

	fmt.Println("Registered 'table' in both 'math' and 'ikea' namespaces")

	mt, err := bison.Instantiate("table", "math")
	check(err)
	defer mt.Close()
	check(mt.SetInt("rows", 10))
	check(mt.SetInt("cols", 5))

	it, err := bison.Instantiate("table", "ikea")
	check(err)
	defer it.Close()
	check(it.SetInt("legs", 4))
	check(it.SetString("material", "oak"))

	rows, _ := mt.GetInt("rows")
	cols, _ := mt.GetInt("cols")
	fmt.Printf("math::table rows=%d cols=%d\n", rows, cols)

	legs, _ := it.GetInt("legs")
	material, _ := it.GetString("material")
	fmt.Printf("ikea::table legs=%d material=%s\n", legs, material)

	furniture, _ := bison.New("Furniture")
	check(furniture.SetInt("warranty", 5))
	check(bison.AddClass(furniture, "", "ikea", nil))

	sofa, _ := bison.New("Sofa")
	check(sofa.SetInt("seats", 3))
	check(bison.AddClass(sofa, "Furniture", "ikea", nil))

	s, err := bison.Instantiate("Sofa", "ikea")
	check(err)
	defer s.Close()
	seats, _ := s.GetInt("seats")
	warranty, _ := s.GetInt("warranty")
	fmt.Printf("ikea::Sofa seats=%d warranty=%d\n", seats, warranty)

	bison.ClearRegistry()
}

// ── Example 8: JSON import ──────────────────────────────────────────────────

func exampleJSON() {
	section("JSON import")
	obj, err := bison.FromJSON(`
		{
		  "name":   "Alice",
		  "age":    30,
		  "score":  9.5,
		  "active": true,
		  "tags":   ["c++", "bison", "serialization"],
		  "address": {"city": "Springfield", "zip": 12345}
		}
	`)
	check(err)
	defer obj.Close()

	name, _ := obj.GetString("name")
	age, _ := obj.GetInt("age")
	active, _ := obj.GetBool("active")
	score, _ := obj.GetFloat("score")
	fmt.Printf("name   : %s\n", name)
	fmt.Printf("age    : %d\n", age)
	fmt.Printf("active : %v\n", active)
	fmt.Printf("score  : %v\n", score)

	addr, err := obj.GetObject("address")
	check(err)
	defer addr.Close()
	city, _ := addr.GetString("city")
	fmt.Printf("city   : %s\n", city)

	tags, err := obj.GetObject("tags")
	check(err)
	defer tags.Close()
	tag0, _ := tags.GetStringAt(0)
	tag2, _ := tags.GetStringAt(2)
	fmt.Printf("tags[0]: %s\n", tag0)
	fmt.Printf("tags[2]: %s\n", tag2)
	fmt.Printf("tag count: %d\n", tags.Size())

	check(obj.SetString("name", "Bob"))
	updatedName, _ := obj.GetString("name")
	fmt.Printf("updated name: %s\n", updatedName)
}

// ── Example 9: YAML import ───────────────────────────────────────────────────

func exampleYAML() {
	section("YAML import")
	obj, err := bison.FromYAML("server:\n  host: localhost\n  port: 8080\ndebug: true\nthreshold: 0.75\ntags:\n  - yaml\n  - bison\n  - example\n")
	check(err)
	defer obj.Close()

	server, err := obj.GetObject("server")
	check(err)
	defer server.Close()
	host, _ := server.GetString("host")
	port, _ := server.GetInt("port")
	fmt.Printf("host      : %s\n", host)
	fmt.Printf("port      : %d\n", port)

	debug, _ := obj.GetBool("debug")
	threshold, _ := obj.GetFloat("threshold")
	fmt.Printf("debug     : %v\n", debug)
	fmt.Printf("threshold : %.2f\n", threshold)

	tags, err := obj.GetObject("tags")
	check(err)
	defer tags.Close()
	tag0, _ := tags.GetStringAt(0)
	tag2, _ := tags.GetStringAt(2)
	fmt.Printf("tags[0]   : %s\n", tag0)
	fmt.Printf("tags[2]   : %s\n", tag2)
	fmt.Printf("tag count : %d\n", tags.Size())
}

// ── Example 10: Reference counting and AddRef ───────────────────────────────

func exampleRefCounting() {
	section("Reference counting and AddRef")
	h, _ := bison.NewAnonymous()
	check(h.SetInt("x", 42))

	alias, err := h.AddRef()
	check(err)
	x, _ := alias.GetInt("x")
	fmt.Printf("alias sees x = %d\n", x)

	check(alias.SetInt("x", 99))
	x, _ = h.GetInt("x")
	fmt.Printf("original after alias mutate: x = %d\n", x)

	alias.Close()
	x, _ = h.GetInt("x")
	fmt.Printf("original after alias release: x = %d\n", x)

	h.Close()
	fmt.Println("Both handles released")
}

func main() {
	exampleHashing()
	exampleScalarFields()
	exampleNestedObjects()
	exampleNumericIndexing()
	exampleMethods()
	exampleInheritance()
	exampleNamespaces()
	exampleJSON()
	exampleYAML()
	exampleRefCounting()
	fmt.Println("\nAll examples completed successfully.")
}

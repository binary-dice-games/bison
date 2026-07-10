"""Detailed, runnable examples for the Bison dynamic-object Python binding.

Mirrors examples/bison_abi_example.cpp feature-for-feature, but using the
Pythonic ``Dynamic`` API (``obj.field = value`` / ``obj["field"] = value``
instead of ``bison_set_int(h, bison_key("field"), value)``).

Run with:  python bindings/python/examples/bison_example.py
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from bison import Dynamic, BisonError, add_class, clear_registry, find_class, instantiate, from_json, from_yaml, key

_section_index = 0


def section(title: str) -> None:
    global _section_index
    _section_index += 1
    print("\n==========================================")
    print(f"  {_section_index}. {title}")
    print("==========================================")


# ── Example 1: Hashing and keys ─────────────────────────────────────────────


def example_hashing():
    section("Hashing and keys")
    k1 = key("velocity")
    k2 = key("velocity")
    print(f'key("velocity") is stable: {k1 == k2}')
    print(f"High bit set on named key: {bool(k1 & 0x80000000)}")
    print(f'"velocity" != "score": {k1 != key("score")}')


# ── Example 2: Scalar field get / set ───────────────────────────────────────


def example_scalar_fields():
    section("Scalar field get / set")
    with Dynamic("Person") as h:
        h.name = "Alice"
        h.age = 30
        h.score = 9.5
        h.active = True

        print("name   :", h.name)
        print("age    :", h.age)
        print("score  :", h.score)
        print("active :", h.active)

        # __getattr__/__getitem__ probe each scalar type in turn (int,
        # float, bool, string, object); type-lock enforcement is visible on
        # __setitem__ instead — see the numeric-indexing example below,
        # where assigning a float to an already-int-typed slot raises
        # BisonError.


# ── Example 3: Nested objects ───────────────────────────────────────────────


def example_nested_objects():
    section("Nested objects")
    person = Dynamic("Person")
    person.name = "Alice"

    address = Dynamic()
    address.street = "123 Main St"
    address.city = "Springfield"

    person.address = address  # bison_set_object increments address's ref-count
    address.release()  # we can release our own copy

    addr_out = person.address
    print("city:", addr_out.city)
    addr_out.release()
    person.release()


# ── Example 4: Numeric (array-like) indexing ────────────────────────────────


def example_numeric_indexing():
    section("Numeric (array-like) indexing")
    with Dynamic() as lst:
        lst[0] = "red"
        lst[1] = "green"
        lst[2] = "blue"
        print("list size :", len(lst))
        print("list[1]   :", lst[1])

    with Dynamic() as scores:
        scores[0] = 10
        scores[1] = 20
        scores[2] = 30
        print("scores[2] :", scores[2])

        try:
            scores[0] = 1.1  # type-locked: int slot rejects float
            print("type mismatch at[0]: unexpected (no error raised)")
        except BisonError as e:
            print("type mismatch at[0]: BISON_ERR_TYPE (expected) ->", e)

    with Dynamic() as fscores:
        fscores[0] = 1.1
        fscores[1] = 2.2
        print("fscores[0]:", fscores[0])


# ── Example 5: Methods — attaching behaviour to objects ─────────────────────


def method_add(self_obj, params, result):
    result.value = params.a + params.b


def method_accumulate(self_obj, params, result):
    total = self_obj.total + params.n
    self_obj.total = total
    result.total = total


def example_methods():
    section("Methods - attaching behaviour to objects")
    calc = Dynamic("Calculator")
    calc.total = 0
    calc.add_method("add", method_add)
    calc.add_method("accumulate", method_accumulate)

    total = calc.add(a=10, b=32)
    print("10 + 32 =", total.value)
    total.release()

    for i in range(1, 6):
        res = calc.accumulate(n=i)
        res.release()

    print("accumulated total (1+2+3+4+5):", calc.total)

    try:
        calc.sqrt()
        print("attribute access to unknown method: unexpected (no error raised)")
    except AttributeError as e:
        print("attribute access to unknown method: AttributeError (expected) ->", e)

    try:
        calc.call("sqrt")
        print("call unknown method: unexpected (no error raised)")
    except BisonError as e:
        print("call unknown method: BISON_ERR_NOT_FOUND (expected) ->", e)

    calc.release()


# ── Example 6: Class hierarchy and inheritance ──────────────────────────────


def method_describe(self_obj, params, result):
    result.text = f"{self_obj.color} shape"


def method_area(self_obj, params, result):
    r = self_obj.radius
    result.area = 3.14159265 * r * r


def example_inheritance():
    section("Class hierarchy and inheritance")
    clear_registry()

    shape = Dynamic("Shape")
    shape.color = "black"
    shape.add_method("describe", method_describe)
    add_class(shape)
    shape.release()

    circle = Dynamic("Circle")
    circle.radius = 1.0
    circle.add_method("area", method_area)
    add_class(circle, parent_name="Shape")
    circle.release()

    c = instantiate("Circle")
    c.radius = 5.0
    c.color = "red"  # overrides the inherited default

    area_result = c.area()
    print(f"Circle area (r=5): {area_result.area:.4f}")
    area_result.release()

    desc_result = c.describe()
    print("Description:", desc_result.text)
    desc_result.release()

    c2 = instantiate("Circle")
    print("Inherited color:", c2.color)
    c2.release()

    dup = Dynamic("Shape")
    try:
        add_class(dup)
        print("Duplicate addClass rejected: False (unexpected)")
    except BisonError:
        print("Duplicate addClass rejected: True")
    dup.release()

    found = find_class("Shape")
    print("Shape found in registry:", found is not None)  # non-owning; do not release

    c.release()
    clear_registry()


# ── Example 7: Namespaces ───────────────────────────────────────────────────


def example_namespaces():
    section("Namespaces - class isolation by unit")
    clear_registry()

    math_table = Dynamic("table")
    math_table.rows = 0
    math_table.cols = 0
    add_class(math_table, ns_name="math")
    math_table.release()

    ikea_table = Dynamic("table")
    ikea_table.legs = 4
    ikea_table.material = "wood"
    add_class(ikea_table, ns_name="ikea")
    ikea_table.release()

    print("Registered 'table' in both 'math' and 'ikea' namespaces")

    mt = instantiate("table", ns_name="math")
    mt.rows = 10
    mt.cols = 5

    it = instantiate("table", ns_name="ikea")
    it.legs = 4
    it.material = "oak"

    print(f"math::table rows={mt.rows} cols={mt.cols}")
    print(f"ikea::table legs={it.legs} material={it.material}")

    furniture = Dynamic("Furniture")
    furniture.warranty = 5
    add_class(furniture, ns_name="ikea")
    furniture.release()

    sofa = Dynamic("Sofa")
    sofa.seats = 3
    add_class(sofa, parent_name="Furniture", ns_name="ikea")
    sofa.release()

    s = instantiate("Sofa", ns_name="ikea")
    print(f"ikea::Sofa seats={s.seats} warranty={s.warranty}")

    s.release()
    mt.release()
    it.release()
    clear_registry()


# ── Example 8: JSON import ──────────────────────────────────────────────────


def example_json():
    section("JSON import")
    obj = from_json(
        """
        {
          "name":   "Alice",
          "age":    30,
          "score":  9.5,
          "active": true,
          "tags":   ["c++", "bison", "serialization"],
          "address": {"city": "Springfield", "zip": 12345}
        }
        """
    )
    print("name   :", obj.name)
    print("age    :", obj.age)
    print("active :", obj.active)
    print("score  :", obj.score)

    addr = obj.address
    print("city   :", addr.city)
    addr.release()

    tags = obj.tags
    print("tags[0]:", tags[0])
    print("tags[2]:", tags[2])
    print("tag count:", len(tags))
    tags.release()

    obj.name = "Bob"
    print("updated name:", obj.name)

    obj.release()


# ── Example 9: YAML import ──────────────────────────────────────────────────


def example_yaml():
    section("YAML import")
    obj = from_yaml(
        "server:\n"
        "  host: localhost\n"
        "  port: 8080\n"
        "debug: true\n"
        "threshold: 0.75\n"
        "tags:\n"
        "  - yaml\n"
        "  - bison\n"
        "  - example\n"
    )

    server = obj.server
    print("host      :", server.host)
    print("port      :", server.port)
    server.release()

    print("debug     :", obj.debug)
    print(f"threshold : {obj.threshold:.2f}")

    tags = obj.tags
    print("tags[0]   :", tags[0])
    print("tags[2]   :", tags[2])
    print("tag count :", len(tags))
    tags.release()

    obj.release()


# ── Example 10: Reference counting and add_ref ──────────────────────────────


def example_ref_counting():
    section("Reference counting and add_ref")
    h = Dynamic()
    h.x = 42

    alias = h.add_ref()
    print("alias sees x =", alias.x)

    alias.x = 99
    print("original after alias mutate: x =", h.x)

    alias.release()
    print("original after alias release: x =", h.x)

    h.release()
    print("Both handles released")


def main():
    example_hashing()
    example_scalar_fields()
    example_nested_objects()
    example_numeric_indexing()
    example_methods()
    example_inheritance()
    example_namespaces()
    example_json()
    example_yaml()
    example_ref_counting()
    print("\nAll examples completed successfully.")


if __name__ == "__main__":
    main()

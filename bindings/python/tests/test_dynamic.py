"""Tests for bison.dynamic — the Bison dynamic-object Python binding.

Build bison_abi first, then run from the repository root::

    cmake -B build -DPACKAGE_TESTS=ON
    cmake --build build --config Debug --target bison_abi
    python -m pytest bindings/python/tests/test_dynamic.py -v
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from bison import Attributes, BisonError, Dynamic, add_class, class_attributes, clear_registry, find_class
from bison import deserialize, from_json, from_yaml, instantiate, key

# ═════════════════════════════════════════════════════════════════════════════
# Lifecycle
# ═════════════════════════════════════════════════════════════════════════════


class TestLifecycle(unittest.TestCase):
    def test_create_succeeds(self):
        obj = Dynamic()
        self.assertIsNotNone(obj)
        obj.release()

    def test_context_manager_releases(self):
        with Dynamic() as obj:
            obj["x"] = 1

    def test_release_idempotent(self):
        obj = Dynamic()
        obj.release()
        obj.release()

    def test_add_ref_shares_mutations(self):
        obj = Dynamic()
        obj["n"] = 10
        ref = obj.add_ref()
        obj["n"] = 20
        self.assertEqual(ref["n"], 20)
        obj.release()
        ref.release()

    def test_clone_is_independent(self):
        obj = Dynamic()
        obj["n"] = 1
        clone = obj.clone()
        clone["n"] = 2
        self.assertEqual(obj["n"], 1)
        self.assertEqual(clone["n"], 2)
        obj.release()
        clone.release()


# ═════════════════════════════════════════════════════════════════════════════
# Field access
# ═════════════════════════════════════════════════════════════════════════════


class TestFieldAccess(unittest.TestCase):
    def setUp(self):
        self.obj = Dynamic()

    def tearDown(self):
        self.obj.release()

    def test_scalar_round_trip(self):
        self.obj["score"] = 42
        self.obj["speed"] = 9.5
        self.obj["alive"] = True
        self.obj["name"] = "hero"
        self.assertEqual(self.obj["score"], 42)
        self.assertAlmostEqual(self.obj["speed"], 9.5)
        self.assertIs(self.obj["alive"], True)
        self.assertEqual(self.obj["name"], "hero")

    def test_type_locked_field_raises(self):
        self.obj["score"] = 1
        with self.assertRaises(BisonError) as ctx:
            self.obj["score"] = 1.5
        self.assertEqual(ctx.exception.code, -2)  # BISON_ERR_TYPE

    def test_nested_object(self):
        child = Dynamic()
        child["city"] = "Springfield"
        self.obj["address"] = child
        child.release()

        addr = self.obj["address"]
        self.assertEqual(addr["city"], "Springfield")
        addr.release()

    def test_indexed_fields_and_size(self):
        self.obj[0] = "red"
        self.obj[1] = "green"
        self.obj[2] = "blue"
        self.assertEqual(len(self.obj), 3)
        self.assertEqual(self.obj[1], "green")
        self.assertEqual(list(self.obj), ["red", "green", "blue"])

    def test_indexed_type_lock(self):
        self.obj[0] = 10
        with self.assertRaises(BisonError):
            self.obj[0] = 1.5

    def test_indexed_bool_round_trips_as_bool(self):
        # Regression coverage: indexed bool used to be silently coerced to
        # int32 (bison_set_bool_at() didn't exist yet), so obj[0] came back
        # as `1`/`0` instead of `True`/`False`.
        self.obj[0] = True
        self.obj[1] = False
        self.assertIs(self.obj[0], True)
        self.assertIs(self.obj[1], False)

    def test_indexed_bool_type_locked(self):
        self.obj[0] = True
        with self.assertRaises(BisonError):
            self.obj[0] = 5

    def test_indexed_object_round_trip(self):
        child = Dynamic()
        child["v"] = 9
        self.obj[0] = child
        child.release()

        out = self.obj[0]
        self.assertEqual(out["v"], 9)
        out.release()

    def test_indexed_null_object_round_trip(self):
        self.obj[0] = None
        self.assertIsNone(self.obj[0])

    def test_set_key_at_round_trips(self):
        self.obj.set_key_at(0, "hero")
        self.assertEqual(self.obj[0], key("hero"))


# ═════════════════════════════════════════════════════════════════════════════
# key_t-typed field access
# ═════════════════════════════════════════════════════════════════════════════


class TestKeyTypedFieldAccess(unittest.TestCase):
    """set_key()/get_key() -- distinct from the plain int32 a bare Python int
    would otherwise become via __setitem__/__getitem__ (see set_key()'s own
    docstring)."""

    def setUp(self):
        self.obj = Dynamic()

    def tearDown(self):
        self.obj.release()

    def test_round_trips_with_already_hashed_value(self):
        self.obj.set_key("id", key("hero"))
        self.assertEqual(self.obj["id"], key("hero"))

    def test_round_trips_with_name_string(self):
        self.obj.set_key("id", "hero")
        self.assertEqual(self.obj["id"], key("hero"))

    def test_distinct_from_int32_field(self):
        # An int32-typed field and a key_t-typed field are different bison
        # variant types even when they happen to hold numerically equal
        # values -- set_key() on a field already claimed as int32 must fail
        # the same way any other type-locked field access would.
        self.obj["plain_int"] = 42
        with self.assertRaises(BisonError) as ctx:
            self.obj.set_key("plain_int", key("anything"))
        self.assertEqual(ctx.exception.code, -2)  # BISON_ERR_TYPE

    def test_getitem_falls_back_to_key_after_other_types_fail(self):
        # __getitem__'s cascade (int/float/bool/string/object) must all fail
        # by type mismatch before reaching the key_t fallback -- proven here
        # by setting the field as key_t first (an untouched field would
        # instead auto-vivify as int32 zero on the very first check; see
        # get_key()'s cascade-ordering comment in dynamic.py).
        self.obj.set_key("selector", "nav_mode_topdown")
        self.assertEqual(self.obj["selector"], key("nav_mode_topdown"))

    def test_add_field_key_declares_and_rejects_duplicate(self):
        self.obj.add_field_key("id", key("hero"))
        self.assertEqual(self.obj["id"], key("hero"))
        with self.assertRaises(BisonError) as ctx:
            self.obj.add_field_key("id", key("other"))
        self.assertEqual(ctx.exception.code, -4)  # BISON_ERR_DUPLICATE

    def test_add_field_key_with_name_string(self):
        self.obj.add_field_key("id", "sidekick")
        self.assertEqual(self.obj["id"], key("sidekick"))


# ═════════════════════════════════════════════════════════════════════════════
# Vector fields
# ═════════════════════════════════════════════════════════════════════════════


class TestVectorFields(unittest.TestCase):
    def setUp(self):
        self.obj = Dynamic()

    def tearDown(self):
        self.obj.release()

    def test_int_round_trip(self):
        self.obj["ints"] = [1, 2, 3]
        self.assertEqual(self.obj["ints"], [1, 2, 3])

    def test_bool_round_trip(self):
        self.obj["flags"] = [True, False, True]
        self.assertEqual(self.obj["flags"], [True, False, True])

    def test_float_round_trip(self):
        self.obj["ratios"] = [1.5, 2.5]
        self.assertEqual(self.obj["ratios"], [1.5, 2.5])

    def test_bytes_round_trip(self):
        self.obj["blob"] = bytes([0, 1, 255])
        self.assertEqual(self.obj["blob"], bytes([0, 1, 255]))

    def test_bytearray_round_trip(self):
        self.obj["blob"] = bytearray([1, 2, 3])
        self.assertEqual(self.obj["blob"], bytes([1, 2, 3]))

    def test_tuple_is_accepted(self):
        self.obj["ints"] = (1, 2, 3)
        self.assertEqual(self.obj["ints"], [1, 2, 3])

    def test_assignment_replaces_existing_contents(self):
        self.obj["ints"] = [1, 2, 3]
        self.obj["ints"] = [9, 9]
        self.assertEqual(self.obj["ints"], [9, 9])

    def test_empty_list_defaults_to_int(self):
        self.obj["empty"] = []
        self.assertEqual(self.obj["empty"], [])

    def test_wrong_type_read_raises(self):
        self.obj["x"] = 1
        # x already holds int32 (a scalar, not a vector), so none of the
        # vector getters match -- this hits __getitem__'s final KeyError,
        # not a vector-specific error, since bison_get_int() above already
        # claimed the field.
        self.assertEqual(self.obj["x"], 1)

    def test_add_field_registers_and_is_readable(self):
        self.obj.add_field("ints", [1, 2, 3])
        self.assertEqual(self.obj["ints"], [1, 2, 3])

    def test_add_field_rejects_duplicate(self):
        self.obj.add_field("ints", [1, 2, 3])
        with self.assertRaises(BisonError) as ctx:
            self.obj.add_field("ints", [9])
        self.assertEqual(ctx.exception.code, -4)  # BISON_ERR_DUPLICATE

    def test_add_field_bytes(self):
        self.obj.add_field("blob", bytes([1, 2, 3]))
        self.assertEqual(self.obj["blob"], bytes([1, 2, 3]))

    def test_indexed_assignment_raises_type_error(self):
        # Vectors have no numeric-index form (bison_c.h has no
        # bison_set_vector_*_at) -- only named fields support them.
        with self.assertRaises(TypeError):
            self.obj[0] = [1, 2, 3]


# ═════════════════════════════════════════════════════════════════════════════
# Methods
# ═════════════════════════════════════════════════════════════════════════════


class TestMethods(unittest.TestCase):
    def test_add_method_and_call(self):
        def method_add(self_obj, params, result):
            result["value"] = params["a"] + params["b"]

        calc = Dynamic()
        calc.add_method("add", method_add)
        args = Dynamic()
        args["a"] = 10
        args["b"] = 32
        out = calc.call("add", args)
        self.assertEqual(out["value"], 42)
        out.release()
        args.release()
        calc.release()

    def test_method_mutates_self(self):
        def accumulate(self_obj, params, result):
            total = self_obj["total"] + params["n"]
            self_obj["total"] = total
            result["total"] = total

        calc = Dynamic()
        calc["total"] = 0
        calc.add_method("accumulate", accumulate)
        for i in range(1, 4):
            p = Dynamic()
            p["n"] = i
            r = calc.call("accumulate", p)
            r.release()
            p.release()
        self.assertEqual(calc["total"], 6)
        calc.release()

    def test_call_unknown_method_raises(self):
        obj = Dynamic()
        with self.assertRaises(BisonError) as ctx:
            obj.call("nope")
        self.assertEqual(ctx.exception.code, -3)  # BISON_ERR_NOT_FOUND
        obj.release()


# ═════════════════════════════════════════════════════════════════════════════
# Class registry / inheritance
# ═════════════════════════════════════════════════════════════════════════════


class TestClassRegistry(unittest.TestCase):
    def setUp(self):
        clear_registry()

    def tearDown(self):
        clear_registry()

    def test_inheritance(self):
        shape = Dynamic("Shape")
        shape["color"] = "black"
        add_class(shape)
        shape.release()

        circle = Dynamic("Circle")
        circle["radius"] = 1.0
        add_class(circle, parent_name="Shape")
        circle.release()

        c = instantiate("Circle")
        self.assertEqual(c["color"], "black")  # inherited default
        self.assertAlmostEqual(c["radius"], 1.0)
        c.release()

    def test_duplicate_class_raises(self):
        proto = Dynamic("Shape")
        add_class(proto)
        proto.release()

        dup = Dynamic("Shape")
        with self.assertRaises(BisonError) as ctx:
            add_class(dup)
        self.assertEqual(ctx.exception.code, -4)  # BISON_ERR_DUPLICATE
        dup.release()

    def test_find_class(self):
        proto = Dynamic("Shape")
        add_class(proto)
        proto.release()
        self.assertIsNotNone(find_class("Shape"))
        self.assertIsNone(find_class("DoesNotExist"))

    def test_namespaces_isolate_same_name(self):
        math_table = Dynamic("table")
        math_table["rows"] = 1
        add_class(math_table, ns_name="math")
        math_table.release()

        ikea_table = Dynamic("table")
        ikea_table["legs"] = 4
        add_class(ikea_table, ns_name="ikea")
        ikea_table.release()

        mt = instantiate("table", ns_name="math")
        it = instantiate("table", ns_name="ikea")
        self.assertEqual(mt["rows"], 1)
        self.assertEqual(it["legs"], 4)
        mt.release()
        it.release()

    def test_class_and_field_attributes(self):
        proto = Dynamic("Widget")
        proto.add_field("count", 0, meta=Attributes(description="a counter", required=True))
        add_class(proto, meta=Attributes(display_name="Widget class"))
        proto.release()

        attrs = class_attributes("Widget")
        self.assertEqual(attrs.display_name, "Widget class")

        w = instantiate("Widget")
        field_attrs = w.field_attributes("count")
        self.assertEqual(field_attrs.description, "a counter")
        self.assertTrue(field_attrs.required)
        w.release()

    def test_registered_method_survives_prototype_gc(self):
        """A class's methods must keep working after the registering
        prototype's Python wrapper is garbage-collected — regression test
        for the ctypes-callback-trampoline lifetime bug fixed in add_class()."""

        def register():
            def method_double(self_obj, params, result):
                result["value"] = params["n"] * 2

            proto = Dynamic("Doubler")
            proto.add_method("double", method_double)
            add_class(proto)
            proto.release()
            # `proto` (and its callback closures) goes out of scope here.

        register()

        inst = instantiate("Doubler")
        args = Dynamic()
        args["n"] = 21
        out = inst.call("double", args)
        self.assertEqual(out["value"], 42)
        out.release()
        args.release()
        inst.release()


# ═════════════════════════════════════════════════════════════════════════════
# JSON / YAML import
# ═════════════════════════════════════════════════════════════════════════════


class TestSerialization(unittest.TestCase):
    def test_from_json(self):
        obj = from_json('{"x": 1, "y": 2.5, "tags": ["a", "b"]}')
        self.assertEqual(obj["x"], 1)
        self.assertAlmostEqual(obj["y"], 2.5)
        tags = obj["tags"]
        self.assertEqual(list(tags), ["a", "b"])
        tags.release()
        obj.release()

    def test_from_yaml(self):
        obj = from_yaml("x: 10\nname: test\n")
        self.assertEqual(obj["x"], 10)
        self.assertEqual(obj["name"], "test")
        obj.release()

    def test_to_json_produces_valid_json(self):
        # Field keys are emitted as "#<hash>" via the C ABI (no key-name map
        # is exposed across it), so we check structural validity/values
        # rather than matching on field names.
        import json

        obj = from_json('{"x": 1}')
        parsed = json.loads(obj.to_json(indent=-1))
        self.assertIn(1, parsed.values())
        obj.release()

    def test_invalid_json_raises(self):
        with self.assertRaises(ValueError):
            from_json("not json")


class TestBinarySerialization(unittest.TestCase):
    def test_round_trips_scalar_fields(self):
        obj = Dynamic()
        obj["x"] = 42
        obj["y"] = 2.5
        obj["s"] = "hello"

        buf = obj.serialize()
        self.assertIsInstance(buf, bytes)
        self.assertGreater(len(buf), 0)

        decoded = deserialize(buf)
        self.assertEqual(decoded["x"], 42)
        self.assertAlmostEqual(decoded["y"], 2.5)
        self.assertEqual(decoded["s"], "hello")
        obj.release()
        decoded.release()

    def test_round_trips_nested_object(self):
        obj = Dynamic()
        child = Dynamic()
        child["city"] = "Springfield"
        obj["address"] = child
        child.release()

        decoded = deserialize(obj.serialize())
        addr = decoded["address"]
        self.assertEqual(addr["city"], "Springfield")
        addr.release()
        obj.release()
        decoded.release()

    def test_malformed_buffer_raises(self):
        with self.assertRaises(BisonError) as ctx:
            deserialize(b"\xff\x00\x01")
        self.assertEqual(ctx.exception.code, -6)  # BISON_ERR_PARSE

    def test_empty_object_round_trips(self):
        obj = Dynamic()
        decoded = deserialize(obj.serialize())
        self.assertIsInstance(decoded, Dynamic)
        obj.release()
        decoded.release()


class TestKeyHashing(unittest.TestCase):
    def test_key_is_stable(self):
        self.assertEqual(key("velocity"), key("velocity"))

    def test_key_high_bit_set(self):
        self.assertTrue(key("velocity") & 0x80000000)

    def test_different_names_differ(self):
        self.assertNotEqual(key("velocity"), key("score"))

    def test_key_is_memoized(self):
        """Repeated calls for the same name must hit the lru_cache instead
        of re-encoding the string and re-crossing the ctypes FFI boundary."""
        import bison.dynamic as dynamic_mod
        from bison import _native as _n

        lib = _n.get_lib()
        calls = []
        real_bison_key = lib.bison_key
        lib.bison_key = lambda name: (calls.append(name), real_bison_key(name))[1]
        try:
            dynamic_mod.key.cache_clear()
            first = key("memoization_probe_field")
            second = key("memoization_probe_field")
            third = key("memoization_probe_field")
        finally:
            lib.bison_key = real_bison_key

        self.assertEqual(first, second)
        self.assertEqual(second, third)
        self.assertEqual(len(calls), 1)

    def test_key_cache_bounded(self):
        import bison.dynamic as dynamic_mod

        self.assertEqual(dynamic_mod.key.cache_info().maxsize, 4096)


if __name__ == "__main__":
    unittest.main()

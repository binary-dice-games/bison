"""
test_bison.py — pytest / unittest tests for the Python bison ctypes binding.

Build ``bison_c`` first, then run the tests from the repository root::

    cmake -B build -DPACKAGE_TESTS=ON
    cmake --build build --config Debug --target bison_c
    python -m pytest python/test_bison.py -v

or::

    python -m unittest python.test_bison
"""

import os
import sys
import unittest

# Ensure the package root is on sys.path so ``import python.bison`` works
# regardless of cwd.
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import python.bison as bison
from python.bison import Dynamic, from_json, from_yaml, key


# ═════════════════════════════════════════════════════════════════════════════
# 1. Lifecycle
# ═════════════════════════════════════════════════════════════════════════════

class TestLifecycle(unittest.TestCase):
    def test_create_succeeds(self):
        obj = Dynamic()
        self.assertIsNotNone(obj)
        obj.release()

    def test_context_manager_releases(self):
        with Dynamic() as obj:
            obj["x"] = 1
        # If release didn't crash, the context manager works.

    def test_release_idempotent(self):
        obj = Dynamic()
        obj.release()
        obj.release()  # should not crash

    def test_add_ref_returns_new_object(self):
        obj = Dynamic()
        obj["v"] = 99
        ref = obj.add_ref()
        self.assertEqual(ref["v"], 99)
        obj.release()
        ref.release()

    def test_add_ref_shared_mutation(self):
        obj = Dynamic()
        obj["n"] = 10
        ref = obj.add_ref()
        obj["n"] = 20
        # Both handles share the same underlying object.
        self.assertEqual(ref["n"], 20)
        obj.release()
        ref.release()


# ═════════════════════════════════════════════════════════════════════════════
# 2. Named field setters / getters
# ═════════════════════════════════════════════════════════════════════════════

class TestNamedFields(unittest.TestCase):
    def setUp(self):
        self.obj = Dynamic()

    def tearDown(self):
        self.obj.release()

    def test_int_round_trip(self):
        self.obj["score"] = 42
        self.assertEqual(self.obj["score"], 42)

    def test_float_round_trip(self):
        self.obj["ratio"] = 3.14
        self.assertAlmostEqual(self.obj["ratio"], 3.14, places=4)

    def test_bool_round_trip_true(self):
        self.obj["flag"] = True
        self.assertTrue(self.obj["flag"])

    def test_bool_round_trip_false(self):
        self.obj["flag"] = False
        self.assertFalse(self.obj["flag"])

    def test_string_round_trip(self):
        self.obj["name"] = "alice"
        self.assertEqual(self.obj["name"], "alice")

    def test_nested_object(self):
        child = Dynamic()
        child["x"] = 7
        self.obj["inner"] = child
        child.release()

        inner = self.obj["inner"]
        self.assertEqual(inner["x"], 7)
        inner.release()

    def test_wrong_type_raises_on_mismatch(self):
        # Set the field to int; assigning float to a typed field must raise.
        self.obj["n"] = 1
        with self.assertRaises(bison.BisonError) as cm:
            self.obj["n"] = 3.14   # type mismatch: field is int32, not float
        self.assertEqual(cm.exception.code, bison.BISON_ERR_TYPE)


# ═════════════════════════════════════════════════════════════════════════════
# 3. Indexed (array-like) fields
# ═════════════════════════════════════════════════════════════════════════════

class TestIndexedFields(unittest.TestCase):
    def setUp(self):
        self.arr = Dynamic()

    def tearDown(self):
        self.arr.release()

    def test_int_at(self):
        self.arr[0] = 10
        self.arr[1] = 20
        self.arr[2] = 30
        self.assertEqual(self.arr[0], 10)
        self.assertEqual(self.arr[1], 20)
        self.assertEqual(self.arr[2], 30)

    def test_float_at(self):
        self.arr[0] = 1.5
        self.assertAlmostEqual(self.arr[0], 1.5, places=4)

    def test_string_at(self):
        self.arr[0] = "hello"
        self.assertEqual(self.arr[0], "hello")

    def test_size(self):
        self.assertEqual(len(self.arr), 0)
        self.arr[0] = 1
        self.arr[1] = 2
        self.assertEqual(len(self.arr), 2)


# ═════════════════════════════════════════════════════════════════════════════
# 4. Import helpers
# ═════════════════════════════════════════════════════════════════════════════

class TestImportHelpers(unittest.TestCase):
    def test_from_json_flat(self):
        with from_json('{"x": 1, "y": 2}') as obj:
            self.assertEqual(obj["x"], 1)
            self.assertEqual(obj["y"], 2)

    def test_from_json_nested(self):
        with from_json('{"a": {"b": 3}}') as obj:
            inner = obj["a"]
            self.assertEqual(inner["b"], 3)
            inner.release()

    def test_from_json_invalid_raises(self):
        with self.assertRaises(ValueError):
            from_json("{broken")

    def test_from_yaml_flat(self):
        with from_yaml("x: 10\nname: test\n") as obj:
            self.assertEqual(obj["x"], 10)
            self.assertEqual(obj["name"], "test")

    def test_from_yaml_bool(self):
        with from_yaml("flag: true\n") as obj:
            self.assertTrue(obj["flag"])

    def test_from_yaml_sequence(self):
        with from_yaml("- 1\n- 2\n- 3\n") as obj:
            self.assertEqual(obj.size(), 3)
            self.assertEqual(obj[0], 1)

    def test_from_yaml_invalid_raises(self):
        with self.assertRaises(ValueError):
            from_yaml("{broken")


# ═════════════════════════════════════════════════════════════════════════════
# 5. key() utility
# ═════════════════════════════════════════════════════════════════════════════

class TestKey(unittest.TestCase):
    def test_high_bit_always_set(self):
        self.assertTrue(key("hello") & 0x80000000)

    def test_deterministic(self):
        self.assertEqual(key("foo"), key("foo"))

    def test_different_strings_differ(self):
        self.assertNotEqual(key("alpha"), key("beta"))


# ═════════════════════════════════════════════════════════════════════════════
# 6. Class registry
# ═════════════════════════════════════════════════════════════════════════════

class TestClassRegistry(unittest.TestCase):
    """We skip this test group in isolated runs because the global registry
    persists across tests and clearing it requires C++ internals.  The
    add_class / find_class path is covered by the C++ bison_c_tests."""

    def test_add_class_does_not_crash(self):
        proto = Dynamic("PyShape_" + str(id(self)))
        try:
            bison.add_class("", proto)
        except bison.BisonError:
            pass  # already registered is acceptable
        finally:
            proto.release()


# ═════════════════════════════════════════════════════════════════════════════
# 7. Methods
# ═════════════════════════════════════════════════════════════════════════════

class TestMethods(unittest.TestCase):
    def test_add_and_call_method(self):
        def increment(self_d: Dynamic, params_d: Dynamic) -> Dynamic:
            n = self_d["n"]
            self_d["n"] = n + 1
            return Dynamic()

        with Dynamic() as obj:
            obj["n"] = 0
            obj.add_method("inc", increment)

            with obj.call("inc", Dynamic()) as _:
                pass  # result not used here
            with obj.call("inc", Dynamic()) as _:
                pass

            self.assertEqual(obj["n"], 2)

    def test_call_missing_method_raises(self):
        with Dynamic() as obj:
            with self.assertRaises(bison.BisonError) as cm:
                obj.call("no_such_method", Dynamic())
            self.assertEqual(cm.exception.code, bison.BISON_ERR_NOT_FOUND)

    def test_add_duplicate_method_raises(self):
        def noop(s, p): return Dynamic()
        with Dynamic() as obj:
            obj.add_method("fn", noop)
            with self.assertRaises(bison.BisonError) as cm:
                obj.add_method("fn", noop)
            self.assertEqual(cm.exception.code, bison.BISON_ERR_DUPLICATE)


if __name__ == "__main__":
    unittest.main()

"""Tests for bison.rmi — the Bison RMI Python binding.

Uses Client.standalone() (in-process dispatch) exclusively so the suite has
no dependency on sockets/ports being available in the test environment.

Build bison_abi first, then run from the repository root::

    cmake -B build -DPACKAGE_TESTS=ON
    cmake --build build --config Debug --target bison_abi
    python -m pytest bindings/python/tests/test_rmi.py -v
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from bison import Dynamic, add_class, clear_registry
from bison.rmi import Client, RmiError


def method_add(self_obj, params, result):
    result["result"] = params["a"] + params["b"]


def method_echo(self_obj, params, result):
    result["value"] = params["value"]


def register_calculator():
    proto = Dynamic("Calculator")
    proto.add_method("add", method_add)
    proto.add_method("echo", method_echo)
    add_class(proto)
    proto.release()


class TestStandaloneRmi(unittest.TestCase):
    def setUp(self):
        clear_registry()
        register_calculator()

    def tearDown(self):
        clear_registry()

    def test_instantiate_and_call(self):
        with Client.standalone() as client:
            with client.instantiate("Calculator") as calc:
                r = calc.call("add", {"a": 10.0, "b": 3.0})
                self.assertEqual(r["result"], 13.0)
                r.release()

    def test_attribute_style_call(self):
        with Client.standalone() as client:
            with client.instantiate("Calculator") as calc:
                r = calc.add(a=1.0, b=2.0)
                self.assertEqual(r["result"], 3.0)
                r.release()

    def test_getitem_setitem_field_patch(self):
        with Client.standalone() as client:
            with client.instantiate("Calculator") as calc:
                calc["label"] = "primary"
                self.assertEqual(calc["label"], "primary")

    def test_get_full_snapshot(self):
        with Client.standalone() as client:
            with client.instantiate("Calculator") as calc:
                calc["label"] = "primary"
                snapshot = calc.get()
                self.assertEqual(snapshot["label"], "primary")
                snapshot.release()

    def test_unknown_method_raises(self):
        with Client.standalone() as client:
            with client.instantiate("Calculator") as calc:
                with self.assertRaises(RmiError):
                    calc.call("does_not_exist")

    def test_multiple_proxies_independent(self):
        with Client.standalone() as client:
            with client.instantiate("Calculator") as a, client.instantiate("Calculator") as b:
                a["label"] = "a"
                b["label"] = "b"
                self.assertEqual(a["label"], "a")
                self.assertEqual(b["label"], "b")


if __name__ == "__main__":
    unittest.main()

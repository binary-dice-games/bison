"""Tests for bison.rmi — the Bison RMI Python binding.

Most tests use Client.standalone() (in-process dispatch) so the suite has no
dependency on sockets/ports being available in the test environment.
TestTcpAuthRmi is the exception -- Server.listen()'s auth parameter is
evaluated during the OP_CONNECT handshake, which standalone sessions skip
entirely, so it needs a real TCP client/server round trip.

Build bison_abi first, then run from the repository root::

    cmake -B build -DPACKAGE_TESTS=ON
    cmake --build build --config Debug --target bison_abi
    python -m pytest bindings/python/tests/test_rmi.py -v
"""

import itertools
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from bison import Dynamic, add_class, clear_registry
from bison.rmi import Client, RmiError, Server


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


_next_tcp_port = itertools.count(30000)


class TestTcpAuthRmi(unittest.TestCase):
    def test_no_auth_set_connect_succeeds(self):
        port = next(_next_tcp_port)
        server = Server.tcp("127.0.0.1", port)
        server.listen()
        client = Client.tcp("127.0.0.1", port)
        try:
            client.connect()
        finally:
            client.release()
            server.stop()
            server.release()

    def test_accepting_callback_receives_payload_and_sets_identity(self):
        port = next(_next_tcp_port)
        server = Server.tcp("127.0.0.1", port)
        seen = {}

        def handler(payload):
            seen["username"] = payload["username"]
            return True, "alice-id"

        server.listen(auth=handler)

        client = Client.tcp("127.0.0.1", port)
        try:
            client.connect({"username": "alice"})
        finally:
            client.release()
            server.stop()
            server.release()

        self.assertEqual(seen.get("username"), "alice")

    def test_rejecting_callback_fails_connect(self):
        port = next(_next_tcp_port)
        server = Server.tcp("127.0.0.1", port)
        server.listen(auth=lambda payload: (False, ""))

        client = Client.tcp("127.0.0.1", port)
        try:
            with self.assertRaises(RmiError):
                client.connect()
        finally:
            client.release()
            server.stop()
            server.release()


if __name__ == "__main__":
    unittest.main()

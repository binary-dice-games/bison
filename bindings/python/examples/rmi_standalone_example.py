"""Standalone RMI example using the Bison Python binding.

Mirrors examples/rmi_abi_standalone_example.cpp: no separate server process —
Client.standalone() dispatches directly to the local (in-process) class
registry. Three worker threads each instantiate a remote Calculator,
perform several operations concurrently, then clean up.

Run with:  python bindings/python/examples/rmi_standalone_example.py
"""

import os
import sys
import threading

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from bison import Dynamic, add_class
from bison.rmi import Client

_print_lock = threading.Lock()


def method_add(self_obj, params, result):
    result["result"] = params["a"] + params["b"]


def method_subtract(self_obj, params, result):
    result["result"] = params["a"] - params["b"]


def method_multiply(self_obj, params, result):
    result["result"] = params["a"] * params["b"]


def method_divide(self_obj, params, result):
    a, b = params["a"], params["b"]
    if b == 0.0:
        result["error"] = "division by zero"
        result["result"] = 0.0
    else:
        result["result"] = a / b


def register_calculator():
    proto = Dynamic("Calculator")
    proto.add_method("add", method_add)
    proto.add_method("subtract", method_subtract)
    proto.add_method("multiply", method_multiply)
    proto.add_method("divide", method_divide)
    add_class(proto)
    proto.release()


def run_client(client_id: int):
    with Client.standalone() as client:
        with client.instantiate("Calculator") as calc:
            with _print_lock:
                print(f"[Client {client_id}] connected")

            a = 10.0 * client_id
            r = calc.add(a=a, b=3.0)
            with _print_lock:
                print(f"[Client {client_id}] add({a:.0f}, 3) = {r['result']:.0f}")
            r.release()

            b = 7.0 * client_id
            r = calc.subtract(a=100.0, b=b)
            with _print_lock:
                print(f"[Client {client_id}] subtract(100, {b:.0f}) = {r['result']:.0f}")
            r.release()

            v = float(client_id)
            r = calc.multiply(a=v, b=v)
            with _print_lock:
                print(f"[Client {client_id}] multiply({v:.0f}, {v:.0f}) = {r['result']:.0f}")
            r.release()

            b = float(client_id)  # non-zero since client_id >= 1
            r = calc.divide(a=42.0, b=b)
            with _print_lock:
                print(f"[Client {client_id}] divide(42, {b:.0f}) = {r['result']:.0f}")
            r.release()

    with _print_lock:
        print(f"[Client {client_id}] done.")


def main():
    register_calculator()
    print("[Server] RMI Calculator registered (standalone in-process mode).")

    threads = [threading.Thread(target=run_client, args=(i,)) for i in range(1, 4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    print("[Server] all clients done.")


if __name__ == "__main__":
    main()

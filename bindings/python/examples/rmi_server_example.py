"""RMI server example using the Bison Python binding.

Mirrors examples/rmi_abi_server_example.cpp. Command-line flags match the
--transport/--host/--port/--name convention used across the other examples.

Run with:  python bindings/python/examples/rmi_server_example.py [--transport=tcp|pipe] [--host=HOST] [--port=PORT] [--name=PATH]
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from bison import Dynamic, add_class
from bison.rmi import Server


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


def main():
    parser = argparse.ArgumentParser(add_help=True)
    parser.add_argument("--transport", choices=["tcp", "pipe"], default="tcp")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=7070)
    parser.add_argument("--name", default="")
    args = parser.parse_args()

    register_calculator()

    if args.transport == "tcp":
        server = Server.tcp(args.host, args.port)
    else:
        server = Server.pipe(args.name)

    server.listen()
    if args.transport == "pipe":
        print(f"[Server] Calculator listening on pipe {args.name}")
    else:
        print(f"[Server] Calculator listening on {args.host}:{args.port}")
    print("[Server] Press Enter to stop...")

    input()

    server.stop()
    server.release()
    print("[Server] stopped.")


if __name__ == "__main__":
    main()

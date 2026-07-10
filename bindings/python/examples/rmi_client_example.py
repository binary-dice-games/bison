"""RMI client example using the Bison Python binding.

Mirrors examples/rmi_abi_client_example.cpp. Run rmi_server_example.py (or
any other Calculator server, e.g. rmi_abi_server_example) with matching
flags before starting this client.

Run with:  python bindings/python/examples/rmi_client_example.py [--transport=tcp|pipe] [--host=HOST] [--port=PORT] [--name=PATH]
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from bison.rmi import Client


def main():
    parser = argparse.ArgumentParser(add_help=True)
    parser.add_argument("--transport", choices=["tcp", "pipe"], default="tcp")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=7070)
    parser.add_argument("--name", default="")
    args = parser.parse_args()

    if args.transport == "tcp":
        client = Client.tcp(args.host, args.port)
    else:
        client = Client.pipe(args.name)

    with client:
        with client.instantiate("Calculator") as calc:
            print("[Client] connected")

            r = calc.add(a=10.0, b=3.0)
            print(f"[Client] add(10, 3) = {r['result']:.0f}")
            r.release()

            r = calc.subtract(a=100.0, b=21.0)
            print(f"[Client] subtract(100, 21) = {r['result']:.0f}")
            r.release()

            r = calc.multiply(a=7.0, b=6.0)
            print(f"[Client] multiply(7, 6) = {r['result']:.0f}")
            r.release()

            r = calc.divide(a=42.0, b=2.0)
            print(f"[Client] divide(42, 2) = {r['result']:.0f}")
            r.release()

    print("[Client] done.")


if __name__ == "__main__":
    main()

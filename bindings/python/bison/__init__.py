"""Pythonic ``ctypes`` bindings for the Bison C ABI (``bison_c.h`` + ``rmi_c.h``).

Quick start::

    from bison import Dynamic

    with Dynamic("Player") as p:
        p["hp"] = 100
        p["name"] = "hero"
        print(p["hp"])   # 100

    from bison.rmi import Client

    with Client.standalone() as client:
        with client.instantiate("Calculator") as calc:
            print(calc.add(a=1.0, b=2.0)["result"])
"""

from .dynamic import (
    Attributes,
    BisonError,
    Dynamic,
    add_class,
    class_attributes,
    clear_registry,
    deserialize,
    find_class,
    from_json,
    from_yaml,
    instantiate,
    key,
)
from .rmi import Client, Future, Proxy, RmiError, Server

__all__ = [
    "Attributes",
    "BisonError",
    "Dynamic",
    "add_class",
    "class_attributes",
    "clear_registry",
    "deserialize",
    "find_class",
    "from_json",
    "from_yaml",
    "instantiate",
    "key",
    "Client",
    "Future",
    "Proxy",
    "RmiError",
    "Server",
]

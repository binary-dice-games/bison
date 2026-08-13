# bison-abi

Python bindings for [Bison](https://github.com/binary-dice-games/bison), a
C++20 library for serializing dynamic, self-describing objects to a compact
binary format, plus RMI (Remote Method Invocation) over TCP, TLS, named-pipe,
in-memory, and stdio transports.

This package is a thin `ctypes` wrapper around `bison_abi`, the project's C
ABI shared library — `pip install` compiles `bison_abi` from source (via
CMake) and ships it inside the installed package, so no separate build step
is needed afterwards.

## Install

```bash
git clone --recurse-submodules https://github.com/binary-dice-games/bison.git
pip install ./bison/bindings/python
```

A C++20 compiler and CMake 3.11+ must be available on the machine running
`pip install` (the build compiles `bison_abi` and its bundled dependencies —
this can take a few minutes). See
[docs/building.md](https://github.com/binary-dice-games/bison/blob/main/docs/building.md)
for platform-specific prerequisites.

## Quick start

```python
from bison import Dynamic

with Dynamic("Player") as p:
    p.hp = 100
    p.name = "hero"
    print(p.hp)   # 100

from bison.rmi import Client

with Client.standalone() as client:
    with client.instantiate("Calculator") as calc:
        result = calc.add(a=1.0, b=2.0)   # calls the "add" remote method
        print(result.result)
```

Full binding documentation (field/method access, RMI transports, auth,
serialization) lives in
[docs/bindings.md](https://github.com/binary-dice-games/bison/blob/main/docs/bindings.md#python-bindingspython).

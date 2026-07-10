# Language Bindings

All bindings wrap the `bison_abi` shared library, which exports both the
dynamic-object API (`bison_c.h`) and the RMI API (`rmi_c.h`). Build it first:

```bash
cmake -B build -DPACKAGE_TESTS=ON
cmake --build build --config Debug --target bison_abi
```

Set `BISON_LIB` to the full path of the shared library if it is not found automatically:

```bash
# Linux:
export BISON_LIB=$(pwd)/build/libbison_abi.so
# macOS:
export BISON_LIB=$(pwd)/build/libbison_abi.dylib
# Windows:
set BISON_LIB=%cd%\build\Debug\bison_abi.dll
```

---

## Python (`bindings/python/`)

Thin `ctypes` wrapper (`bindings/python/bison/`) exposing both APIs through
idiomatic Python syntax instead of raw C calls — e.g. `params["a"] = 10.0`
instead of `bison_set_float(params, bison_key("a"), 10.0f)`. Every handle is
wrapped in an RAII object (`Dynamic`, `Client`, `Server`, `Proxy`, `Future`);
none of the `bison_*_release` / `rmi_*_release` functions need to be called
directly — use `.release()`, a `with` block, or let the wrapper's
`__del__` clean up. No installation needed — import directly.

**Requirements:** Python 3.x, `pytest` (optional, for tests)

```bash
# Run examples:
python bindings/python/examples/bison_example.py
python bindings/python/examples/rmi_standalone_example.py
python bindings/python/examples/rmi_server_example.py --transport=tcp --port=7070   # separate terminal
python bindings/python/examples/rmi_client_example.py --transport=tcp --port=7070

# Run tests:
python -m pytest bindings/python/tests -v
python -m unittest discover -s bindings/python/tests
```

Quick-start snippet:

```python
from bison import Dynamic

with Dynamic("Player") as p:
    p["hp"] = 100
    p["name"] = "hero"
    print(p["hp"])   # 100

from bison.rmi import Client

with Client.standalone() as client:
    with client.instantiate("Calculator") as calc:
        result = calc.add(a=1.0, b=2.0)   # calls the "add" remote method
        print(result["result"])
```

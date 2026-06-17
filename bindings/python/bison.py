"""
bison.py — Python ctypes binding for the Bison dynamic-object library.

This module wraps the ``libbison_c`` shared library (``bison_c.h`` / ``bison_c.cpp``)
using ``ctypes``. It provides a Pythonic, object-oriented API on top of the
opaque C handle, hiding all reference-counting details.

Repository usage
----------------
Build the native shared library before importing this module::

    cmake -B build -DPACKAGE_TESTS=ON
    cmake --build build --config Debug --target bison_c

From the repository root, run the examples and tests with::

    python bindings/python/examples.py
    python -m pytest bindings/python/test_bison.py -v

If the shared library is not in the default ``build/`` location, set the
``BISON_LIB`` environment variable to the full path of ``bison_c.dll``,
``libbison_c.so``, or ``libbison_c.dylib`` before importing this module.

Quick start
-----------
>>> import sys, os
>>> sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
>>> from python.bison import Dynamic, from_json, from_yaml, add_class
>>> obj = Dynamic()
>>> obj["score"] = 42
>>> obj["name"] = "alice"
>>> print(obj["score"])   # 42
>>> print(obj["name"])    # alice
>>> obj.release()         # must be called when done

Automatic resource management
------------------------------
``Dynamic`` instances can also be used as context managers, which ensures the
handle is always released even if an exception is raised::

    with Dynamic() as obj:
        obj["x"] = 3.14
        print(obj["x"])

Method registration
-------------------
Register Python callables as named methods on any ``Dynamic`` object::

    def greet(self, params):
        result = Dynamic()
        result["greeting"] = "hello, " + self["name"]
        return result

    obj = Dynamic()
    obj["name"] = "world"
    obj.add_method("greet", greet)
    ret = obj.call("greet", Dynamic())
    print(ret["greeting"])   # hello, world
    ret.release()

Thread safety
-------------
The same thread-safety guarantees as the underlying C++ library apply: the
global class registry is protected by a shared mutex; per-object access is the
caller's responsibility.
"""

import ctypes
import os
import sys
import threading
from typing import Any, Callable, Optional

# ─── Library loading ──────────────────────────────────────────────────────────

def _find_library() -> str:
    """Search for the ``libbison_c`` shared library.

    Looks in (in order):
    1. The ``BISON_LIB`` environment variable (if set).
    2. A ``build/`` directory adjacent to this file's parent.
    3. The system library path (``ctypes.util.find_library``).
    """
    env_path = os.environ.get("BISON_LIB")
    if env_path:
        return env_path

    # Typical layout: <repo>/bindings/python/bison.py -> <repo>/build/libbison_c.so
    here = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.dirname(os.path.dirname(here))
    candidates = [
        os.path.join(repo_root, "build", "libbison_c.so"),    # Linux
        os.path.join(repo_root, "build", "libbison_c.dylib"), # macOS
        os.path.join(repo_root, "build", "Release", "bison_c.dll"),  # Windows
        os.path.join(repo_root, "build", "Debug",   "bison_c.dll"),
    ]
    for path in candidates:
        if os.path.isfile(path):
            return path

    import ctypes.util
    found = ctypes.util.find_library("bison_c")
    if found:
        return found

    raise OSError(
        "libbison_c not found.  Build the project first and/or set the "
        "BISON_LIB environment variable to the path of libbison_c.so/.dylib/.dll."
    )


_lib_lock = threading.Lock()
_lib: Optional[ctypes.CDLL] = None


def _get_lib() -> ctypes.CDLL:
    """Return the loaded shared library (singleton, thread-safe)."""
    global _lib
    if _lib is None:
        with _lib_lock:
            if _lib is None:
                _lib = ctypes.CDLL(_find_library())
                _setup_signatures(_lib)
    return _lib


# ─── C type aliases ───────────────────────────────────────────────────────────

_Handle = ctypes.c_void_p   # bison_handle (opaque void*)
_Error  = ctypes.c_int       # bison_error

BISON_OK            =  0
BISON_ERR_NULL      = -1
BISON_ERR_TYPE      = -2
BISON_ERR_NOT_FOUND = -3
BISON_ERR_DUPLICATE = -4
BISON_ERR_EXCEPTION = -5
BISON_ERR_PARSE     = -6

_ERROR_MESSAGES = {
    BISON_ERR_NULL:      "Null handle or pointer",
    BISON_ERR_TYPE:      "Field type mismatch",
    BISON_ERR_NOT_FOUND: "Method or field not found",
    BISON_ERR_DUPLICATE: "Duplicate class or method",
    BISON_ERR_EXCEPTION: "Internal C++ exception",
    BISON_ERR_PARSE:     "Parse error (JSON / YAML)",
}

# C method callback: (self, params, result, user_ptr) -> void
_MethodFnType = ctypes.CFUNCTYPE(
    None,          # return type: void
    _Handle,       # self
    _Handle,       # params
    _Handle,       # result
    ctypes.c_void_p,  # user
)


def _setup_signatures(lib: ctypes.CDLL) -> None:
    """Bind argument and return types to every exported function."""
    # Lifecycle
    lib.bison_create.restype  = _Handle
    lib.bison_create.argtypes = [ctypes.c_uint32]

    lib.bison_instantiate.restype  = _Handle
    lib.bison_instantiate.argtypes = [ctypes.c_uint32]

    lib.bison_add_ref.restype  = _Handle
    lib.bison_add_ref.argtypes = [_Handle]

    lib.bison_release.restype  = None
    lib.bison_release.argtypes = [_Handle]

    # Import
    lib.bison_from_json.restype  = _Handle
    lib.bison_from_json.argtypes = [ctypes.c_char_p]

    lib.bison_from_yaml.restype  = _Handle
    lib.bison_from_yaml.argtypes = [ctypes.c_char_p]

    # Class registry
    lib.bison_add_class.restype  = _Error
    lib.bison_add_class.argtypes = [ctypes.c_uint32, _Handle]

    lib.bison_find_class.restype  = _Handle
    lib.bison_find_class.argtypes = [ctypes.c_uint32]

    lib.bison_find_class_ns.restype  = _Handle
    lib.bison_find_class_ns.argtypes = [ctypes.c_uint32, ctypes.c_uint32]

    # Setters (named)
    for fn_name in ("bison_set_int", "bison_set_float", "bison_set_bool"):
        fn = getattr(lib, fn_name)
        fn.restype  = _Error
        fn.argtypes = [_Handle, ctypes.c_uint32, ctypes.c_int32]
    lib.bison_set_float.argtypes = [_Handle, ctypes.c_uint32, ctypes.c_float]
    lib.bison_set_string.restype  = _Error
    lib.bison_set_string.argtypes = [_Handle, ctypes.c_uint32, ctypes.c_char_p]
    lib.bison_set_object.restype  = _Error
    lib.bison_set_object.argtypes = [_Handle, ctypes.c_uint32, _Handle]

    # Setters (indexed)
    lib.bison_set_int_at.restype   = _Error
    lib.bison_set_int_at.argtypes  = [_Handle, ctypes.c_size_t, ctypes.c_int32]
    lib.bison_set_float_at.restype  = _Error
    lib.bison_set_float_at.argtypes = [_Handle, ctypes.c_size_t, ctypes.c_float]
    lib.bison_set_string_at.restype  = _Error
    lib.bison_set_string_at.argtypes = [_Handle, ctypes.c_size_t, ctypes.c_char_p]

    # Getters (named)
    lib.bison_get_int.restype   = _Error
    lib.bison_get_int.argtypes  = [_Handle, ctypes.c_uint32, ctypes.POINTER(ctypes.c_int32)]
    lib.bison_get_float.restype  = _Error
    lib.bison_get_float.argtypes = [_Handle, ctypes.c_uint32, ctypes.POINTER(ctypes.c_float)]
    lib.bison_get_bool.restype   = _Error
    lib.bison_get_bool.argtypes  = [_Handle, ctypes.c_uint32, ctypes.POINTER(ctypes.c_int)]
    lib.bison_get_string.restype  = _Error
    lib.bison_get_string.argtypes = [
        _Handle, ctypes.c_uint32, ctypes.c_char_p, ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_size_t),
    ]
    lib.bison_get_object.restype  = _Error
    lib.bison_get_object.argtypes = [_Handle, ctypes.c_uint32, ctypes.POINTER(_Handle)]

    # Getters (indexed)
    lib.bison_get_int_at.restype   = _Error
    lib.bison_get_int_at.argtypes  = [_Handle, ctypes.c_size_t, ctypes.POINTER(ctypes.c_int32)]
    lib.bison_get_float_at.restype  = _Error
    lib.bison_get_float_at.argtypes = [_Handle, ctypes.c_size_t, ctypes.POINTER(ctypes.c_float)]
    lib.bison_get_string_at.restype  = _Error
    lib.bison_get_string_at.argtypes = [
        _Handle, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_size_t),
    ]

    lib.bison_size.restype  = ctypes.c_size_t
    lib.bison_size.argtypes = [_Handle]

    # Methods
    lib.bison_add_method.restype  = _Error
    lib.bison_add_method.argtypes = [_Handle, ctypes.c_uint32, _MethodFnType,
                                     ctypes.c_void_p]

    lib.bison_call.restype  = _Error
    lib.bison_call.argtypes = [_Handle, ctypes.c_uint32, _Handle, ctypes.POINTER(_Handle)]

    # Utility
    lib.bison_key.restype  = ctypes.c_uint32
    lib.bison_key.argtypes = [ctypes.c_char_p]


# ─── Error helpers ────────────────────────────────────────────────────────────

class BisonError(RuntimeError):
    """Raised when a bison C API call returns a non-zero error code."""
    def __init__(self, code: int, context: str = ""):
        msg = _ERROR_MESSAGES.get(code, f"Unknown error {code}")
        super().__init__(f"{context}: {msg}" if context else msg)
        self.code = code


def _check(rc: int, context: str = "") -> None:
    """Raise :class:`BisonError` if *rc* is non-zero."""
    if rc != BISON_OK:
        raise BisonError(rc, context)


# ─── bison_key helper ─────────────────────────────────────────────────────────

def key(name: str) -> int:
    """Return the 32-bit FNV-1a hash of *name* (same as ``"name"_key`` in C++).

    >>> key("hello") & 0x80000000
    2147483648
    """
    return _get_lib().bison_key(name.encode())


# ─── Dynamic class ─────────────────────────────────────────────────────────────

class Dynamic:
    """A reference-counted dynamic object.

    ``Dynamic`` wraps an opaque ``bison_handle`` and provides a Pythonic
    interface for reading and writing fields, calling methods, and managing
    object lifetime.

    Parameters
    ----------
    klass_name : str, optional
        Class name for the new object (default: anonymous, key = 0).
    _handle : int, optional
        Internal: adopt an already-allocated handle instead of creating a new
        object.  Used by factory functions such as :func:`from_json`.

    Examples
    --------
    Basic field access::

        obj = Dynamic()
        obj["hp"]    = 100
        obj["speed"] = 9.5
        print(obj["hp"])     # 100
        print(obj["speed"])  # 9.5
        obj.release()

    Context manager::

        with Dynamic("Player") as player:
            player["name"] = "alice"
    """

    __slots__ = ("_handle", "_lib", "_owned", "_released", "_callbacks")

    def __init__(self, klass_name: str = "", *, _handle: Optional[int] = None,
                 _owned: bool = True):
        self._lib = _get_lib()
        # _owned=True  → this instance holds an owning reference; release() will
        #                 call bison_release to decrement the shared_ptr refcount.
        # _owned=False → this instance is a non-owning view of a handle managed
        #                 by the C library (e.g. self/params handles inside a
        #                 method callback); release() is a no-op.
        self._owned = _owned
        self._released = False
        # Keep Python references to ctypes callbacks alive for the lifetime of
        # this object so the garbage collector doesn't free them prematurely.
        self._callbacks: list = []

        if _handle is not None:
            self._handle = _handle
        elif klass_name:
            h = self._lib.bison_create(key(klass_name))
            if not h:
                raise MemoryError("bison_create failed")
            self._handle = h
        else:
            h = self._lib.bison_create(0)
            if not h:
                raise MemoryError("bison_create failed")
            self._handle = h

    # ── Context manager ──────────────────────────────────────────────────────

    def __enter__(self) -> "Dynamic":
        return self

    def __exit__(self, *_) -> None:
        self.release()

    def release(self) -> None:
        """Release the underlying handle (decrement ref-count).

        Safe to call multiple times; subsequent calls are no-ops.
        Non-owning instances (created inside method callbacks) are silently
        skipped.
        """
        if self._owned and not self._released:
            self._released = True
            self._lib.bison_release(self._handle)

    def add_ref(self) -> "Dynamic":
        """Return a new :class:`Dynamic` sharing ownership of the same object.

        The caller is responsible for releasing the returned instance.
        """
        h = self._lib.bison_add_ref(self._handle)
        if not h:
            raise RuntimeError("bison_add_ref failed")
        return Dynamic(_handle=h)

    # ── Field access ─────────────────────────────────────────────────────────

    def __setitem__(self, name: Any, value: Any) -> None:
        """Set a named field or indexed element.

        The Python type of *value* determines which setter is called:

        ============ ================
        Python type  C field type
        ============ ================
        ``int``      ``int32_t``
        ``float``    ``float``
        ``bool``     ``bool``
        ``str``      ``std::string``
        :class:`Dynamic` nested object
        ============ ================

        *name* may be a ``str`` (named field) or an ``int`` (indexed element).
        """
        lib = self._lib
        h = self._handle
        if isinstance(name, int):
            if isinstance(value, bool):
                value = int(value)
            if isinstance(value, int):
                _check(lib.bison_set_int_at(h, name, value), f"set_int_at[{name}]")
            elif isinstance(value, float):
                _check(lib.bison_set_float_at(h, name, ctypes.c_float(value)), f"set_float_at[{name}]")
            elif isinstance(value, str):
                _check(lib.bison_set_string_at(h, name, value.encode()), f"set_string_at[{name}]")
            else:
                raise TypeError(f"Unsupported value type for indexed field: {type(value)}")
        else:
            key_h = key(str(name))
            if isinstance(value, bool):
                _check(lib.bison_set_bool(h, key_h, int(value)), f"set_bool[{name}]")
            elif isinstance(value, int):
                _check(lib.bison_set_int(h, key_h, value), f"set_int[{name}]")
            elif isinstance(value, float):
                _check(lib.bison_set_float(h, key_h, ctypes.c_float(value)), f"set_float[{name}]")
            elif isinstance(value, str):
                _check(lib.bison_set_string(h, key_h, value.encode()), f"set_string[{name}]")
            elif isinstance(value, Dynamic):
                _check(lib.bison_set_object(h, key_h, value._handle), f"set_object[{name}]")
            elif value is None:
                _check(lib.bison_set_object(h, key_h, None), f"set_object_null[{name}]")
            else:
                raise TypeError(f"Unsupported value type: {type(value)}")

    def __getitem__(self, name: Any) -> Any:
        """Get a named field or indexed element.

        The returned Python type mirrors the stored C field type.
        A nested ``dynamic`` is returned as a new :class:`Dynamic` instance
        (caller must release it).

        *name* may be a ``str`` or an ``int``.
        """
        lib = self._lib
        h = self._handle

        if isinstance(name, int):
            return self._get_at(name)

        key_h = key(str(name))

        # Try int32 first, then float, then bool, then string, then object.
        v_int = ctypes.c_int32(0)
        rc = lib.bison_get_int(h, key_h, ctypes.byref(v_int))
        if rc == BISON_OK:
            return int(v_int.value)

        v_float = ctypes.c_float(0.0)
        rc = lib.bison_get_float(h, key_h, ctypes.byref(v_float))
        if rc == BISON_OK:
            return float(v_float.value)

        v_bool = ctypes.c_int(0)
        rc = lib.bison_get_bool(h, key_h, ctypes.byref(v_bool))
        if rc == BISON_OK:
            return bool(v_bool.value)

        len_out = ctypes.c_size_t(0)
        rc = lib.bison_get_string(h, key_h, None, 0, ctypes.byref(len_out))
        if rc == BISON_OK:
            buf = ctypes.create_string_buffer(len_out.value + 1)
            lib.bison_get_string(h, key_h, buf, len_out.value + 1, None)
            return buf.value.decode()

        child_h = _Handle(0)
        rc = lib.bison_get_object(h, key_h, ctypes.byref(child_h))
        if rc == BISON_OK:
            return Dynamic(_handle=child_h.value)

        raise KeyError(f"Field '{name}' not found or has an unsupported type")

    def _get_at(self, index: int) -> Any:
        """Get a field by numeric index."""
        lib = self._lib
        h = self._handle

        v_int = ctypes.c_int32(0)
        if lib.bison_get_int_at(h, index, ctypes.byref(v_int)) == BISON_OK:
            return int(v_int.value)

        v_float = ctypes.c_float(0.0)
        if lib.bison_get_float_at(h, index, ctypes.byref(v_float)) == BISON_OK:
            return float(v_float.value)

        len_out = ctypes.c_size_t(0)
        rc = lib.bison_get_string_at(h, index, None, 0, ctypes.byref(len_out))
        if rc == BISON_OK:
            buf = ctypes.create_string_buffer(len_out.value + 1)
            lib.bison_get_string_at(h, index, buf, len_out.value + 1, None)
            return buf.value.decode()

        raise IndexError(f"Index {index} not found or has an unsupported type")

    # ── Array helpers ─────────────────────────────────────────────────────────

    def size(self) -> int:
        """Return the number of array-like (numeric-key) elements."""
        return self._lib.bison_size(self._handle)

    def __len__(self) -> int:
        return self.size()

    # ── Methods ───────────────────────────────────────────────────────────────

    def add_method(self, name: str, fn: Callable[["Dynamic", "Dynamic"], "Dynamic"]) -> None:
        """Register a Python callable as a named method.

        *fn* must have the signature ``fn(self: Dynamic, params: Dynamic) -> Dynamic``.
        The returned :class:`Dynamic` is the method's return value; the caller
        must NOT release it (the library takes ownership).

        The callback reference is kept alive for the lifetime of this
        :class:`Dynamic` instance.

        Parameters
        ----------
        name : str
            Method name.
        fn : callable
            Python function implementing the method.

        Raises
        ------
        BisonError
            If the method name is already registered (BISON_ERR_DUPLICATE).
        """
        lib = self._lib

        def c_callback(self_h: int, params_h: int, result_h: int,
                        user: ctypes.c_void_p) -> None:
            # Wrap the raw handles as non-owning Dynamic views (_owned=False).
            # The handles are managed by the C++ layer; we must not release them.
            self_dyn   = Dynamic(_handle=self_h,   _owned=False)
            params_dyn = Dynamic(_handle=params_h, _owned=False)
            result_dyn = Dynamic(_handle=result_h, _owned=False)

            try:
                ret = fn(self_dyn, params_dyn)
                # Copy all indexed fields from ret into result.
                for i in range(ret.size()):
                    result_dyn[i] = ret[i]
                # Named fields: iterate common string keys (best effort).
                # Users can also populate result_dyn directly inside fn.
            except Exception:
                pass  # Swallow Python exceptions (C ABI must not throw)

        c_fn = _MethodFnType(c_callback)
        # Keep Python reference alive.
        self._callbacks.append(c_fn)

        rc = lib.bison_add_method(self._handle, key(name), c_fn, None, None)
        _check(rc, f"add_method({name!r})")

    def call(self, name: str, params: "Dynamic") -> "Dynamic":
        """Invoke a named method on this object.

        Parameters
        ----------
        name : str
            Method name.
        params : Dynamic
            Argument object (may be an empty ``Dynamic()``).

        Returns
        -------
        Dynamic
            The return value; caller must release it.

        Raises
        ------
        BisonError
            If the method is not found (BISON_ERR_NOT_FOUND).
        """
        result_h = _Handle(0)
        rc = self._lib.bison_call(
            self._handle, key(name), params._handle, ctypes.byref(result_h)
        )
        _check(rc, f"call({name!r})")
        return Dynamic(_handle=result_h.value)

    # ── Repr ──────────────────────────────────────────────────────────────────

    def __repr__(self) -> str:
        invalid = (self._owned and self._released) or not self._handle
        state = "released" if invalid else f"handle=0x{self._handle:x}"
        n = 0 if invalid else self.size()
        return f"Dynamic({state}, size={n})"


# ─── Factory functions ────────────────────────────────────────────────────────

def from_json(text: str) -> Dynamic:
    """Parse a JSON string and return the root object as a :class:`Dynamic`.

    Parameters
    ----------
    text : str
        Valid JSON string.

    Returns
    -------
    Dynamic
        The decoded root object (caller must release).

    Raises
    ------
    ValueError
        If *text* is not valid JSON or parsing fails.

    Examples
    --------
    >>> obj = from_json('{"x": 1, "y": 2}')
    >>> obj["x"]
    1
    >>> obj.release()
    """
    h = _get_lib().bison_from_json(text.encode())
    if not h:
        raise ValueError("from_json: invalid or unsupported JSON")
    return Dynamic(_handle=h)


def from_yaml(text: str) -> Dynamic:
    """Parse a YAML string and return the root object as a :class:`Dynamic`.

    Parameters
    ----------
    text : str
        Valid YAML string (UTF-8).

    Returns
    -------
    Dynamic
        The decoded root object (caller must release).

    Raises
    ------
    ValueError
        If *text* is not valid YAML or parsing fails.

    Examples
    --------
    >>> obj = from_yaml("x: 10\\nname: test\\n")
    >>> obj["x"]
    10
    >>> obj.release()
    """
    h = _get_lib().bison_from_yaml(text.encode())
    if not h:
        raise ValueError("from_yaml: invalid or unsupported YAML")
    return Dynamic(_handle=h)


def add_class(parent_name: str, prototype: Dynamic) -> None:
    """Register *prototype* as a named class in the global class registry.

    Parameters
    ----------
    parent_name : str
        Name of the parent class (empty string or ``""`` for a root class).
    prototype : Dynamic
        Object whose ``__class`` field is set to the desired class name.

    Raises
    ------
    BisonError
        If a class with the same name is already registered.

    Examples
    --------
    >>> vehicle = Dynamic("Vehicle")
    >>> vehicle["wheels"] = 4
    >>> add_class("", vehicle)
    >>> car = Dynamic("Car")
    >>> add_class("Vehicle", car)
    >>> vehicle.release()
    >>> car.release()
    """
    parent_key = key(parent_name) if parent_name else 0
    rc = _get_lib().bison_add_class(parent_key, prototype._handle)
    _check(rc, f"add_class(parent={parent_name!r})")

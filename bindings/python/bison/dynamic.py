"""Pythonic RAII wrapper around ``bison_c.h`` — the Bison dynamic-object C ABI.

``Dynamic`` wraps an opaque ``bison_handle`` and exposes it through standard
Python syntax: ``obj["field"] = value``, ``len(obj)``, ``for v in obj``,
``with Dynamic() as obj:`` etc.  Reference counting is handled automatically;
callers never need to call ``bison_release`` themselves.  Dict- and
attribute-style access are both supported: ``obj["field"]``/``obj.field``
project a field, and ``obj.some_method(a=1, b=2)`` invokes a registered
method by name (equivalent to ``obj.call("some_method", {"a": 1, "b": 2})``).
"""

import ctypes
from dataclasses import dataclass
from typing import Any, Callable, Iterator, Optional

from . import _native as _n

__all__ = [
    "BisonError",
    "Attributes",
    "Dynamic",
    "key",
    "from_json",
    "from_yaml",
    "add_class",
    "find_class",
    "instantiate",
    "clear_registry",
    "class_attributes",
]


class BisonError(RuntimeError):
    """Raised when a ``bison_*`` C API call returns a non-zero error code."""

    def __init__(self, code: int, context: str = ""):
        msg = _n.BISON_ERROR_MESSAGES.get(code, f"Unknown error {code}")
        super().__init__(f"{context}: {msg}" if context else msg)
        self.code = code


def _check(rc: int, context: str = "") -> None:
    if rc != _n.BISON_OK:
        raise BisonError(rc, context)


def key(name: str) -> int:
    """Return the 32-bit FNV-1a hash of *name* (same as ``"name"_key`` in C++)."""
    return _n.get_lib().bison_key(name.encode())


@dataclass
class Attributes:
    """Optional display/documentation metadata for a class, field, or method."""

    display_name: Optional[str] = None
    description: Optional[str] = None
    category: Optional[str] = None
    obsolete: bool = False
    obsolete_message: Optional[str] = None
    required: bool = False

    def _to_c(self) -> _n.CAttributes:
        def enc(s: Optional[str]) -> Optional[bytes]:
            return s.encode() if s is not None else None

        return _n.CAttributes(
            display_name=enc(self.display_name),
            description=enc(self.description),
            category=enc(self.category),
            obsolete=int(self.obsolete),
            obsolete_message=enc(self.obsolete_message),
            required=int(self.required),
        )

    @classmethod
    def _from_c(cls, c: _n.CAttributes) -> "Attributes":
        def dec(s: Optional[bytes]) -> Optional[str]:
            return s.decode() if s is not None else None

        return cls(
            display_name=dec(c.display_name),
            description=dec(c.description),
            category=dec(c.category),
            obsolete=bool(c.obsolete),
            obsolete_message=dec(c.obsolete_message),
            required=bool(c.required),
        )


def _meta_ptr(meta: Optional[Attributes]):
    if meta is None:
        return None
    c = meta._to_c()
    return ctypes.pointer(c)


class Dynamic:
    """A reference-counted Bison dynamic object.

    Supports item access (``obj["field"]``/``obj[0]``), ``len()``,
    iteration over array-like elements, and use as a context manager.
    """

    __slots__ = ("_handle", "_lib", "_owned", "_released", "_callbacks")

    def __init__(self, klass_name: str = "", *, _handle: Optional[int] = None, _owned: bool = True):
        self._lib = _n.get_lib()
        # _owned=True  -> this instance holds a reference; release() decrements it.
        # _owned=False -> non-owning view (e.g. self/params/result inside a method
        #                 callback, or a class-registry lookup); release() is a no-op.
        self._owned = _owned
        self._released = False
        self._callbacks: list = []  # keep ctypes callback objects alive

        if _handle is not None:
            self._handle = _handle
        else:
            h = self._lib.bison_create(key(klass_name) if klass_name else 0)
            if not h:
                raise MemoryError("bison_create failed")
            self._handle = h

    # ── Context manager / lifecycle ─────────────────────────────────────────

    def __enter__(self) -> "Dynamic":
        return self

    def __exit__(self, *_exc) -> None:
        self.release()

    def release(self) -> None:
        """Release the underlying handle. Safe to call multiple times."""
        if self._owned and not self._released:
            self._released = True
            self._lib.bison_release(self._handle)

    def __del__(self):
        try:
            self.release()
        except Exception:
            pass

    def add_ref(self) -> "Dynamic":
        """Return a new :class:`Dynamic` sharing ownership of the same object."""
        h = self._lib.bison_add_ref(self._handle)
        if not h:
            raise RuntimeError("bison_add_ref failed")
        return Dynamic(_handle=h)

    def clone(self) -> "Dynamic":
        """Return a deep copy as a new, independently-owned :class:`Dynamic`."""
        h = self._lib.bison_clone(self._handle)
        if not h:
            raise RuntimeError("bison_clone failed")
        return Dynamic(_handle=h)

    # ── Field access ─────────────────────────────────────────────────────────

    def __setitem__(self, name: Any, value: Any) -> None:
        lib, h = self._lib, self._handle
        if isinstance(name, int):
            if isinstance(value, bool):
                value = int(value)
            if isinstance(value, int):
                _check(lib.bison_set_int_at(h, name, value), f"set_int_at[{name}]")
            elif isinstance(value, float):
                _check(lib.bison_set_float_at(h, name, value), f"set_float_at[{name}]")
            elif isinstance(value, str):
                _check(lib.bison_set_string_at(h, name, value.encode()), f"set_string_at[{name}]")
            else:
                raise TypeError(f"Unsupported value type for indexed field: {type(value)}")
            return

        k = key(str(name))
        if isinstance(value, bool):
            _check(lib.bison_set_bool(h, k, int(value)), f"set_bool[{name}]")
        elif isinstance(value, int):
            _check(lib.bison_set_int(h, k, value), f"set_int[{name}]")
        elif isinstance(value, float):
            _check(lib.bison_set_float(h, k, value), f"set_float[{name}]")
        elif isinstance(value, str):
            _check(lib.bison_set_string(h, k, value.encode()), f"set_string[{name}]")
        elif isinstance(value, Dynamic):
            _check(lib.bison_set_object(h, k, value._handle), f"set_object[{name}]")
        elif value is None:
            _check(lib.bison_set_object(h, k, None), f"set_object_null[{name}]")
        else:
            raise TypeError(f"Unsupported value type: {type(value)}")

    def __getitem__(self, name: Any) -> Any:
        if isinstance(name, int):
            return self._get_at(name)

        lib, h = self._lib, self._handle
        k = key(str(name))

        v_int = ctypes.c_int32(0)
        if lib.bison_get_int(h, k, ctypes.byref(v_int)) == _n.BISON_OK:
            return int(v_int.value)

        v_float = ctypes.c_float(0.0)
        if lib.bison_get_float(h, k, ctypes.byref(v_float)) == _n.BISON_OK:
            return float(v_float.value)

        v_bool = ctypes.c_int(0)
        if lib.bison_get_bool(h, k, ctypes.byref(v_bool)) == _n.BISON_OK:
            return bool(v_bool.value)

        len_out = ctypes.c_size_t(0)
        if lib.bison_get_string(h, k, None, 0, ctypes.byref(len_out)) == _n.BISON_OK:
            buf = ctypes.create_string_buffer(len_out.value + 1)
            lib.bison_get_string(h, k, buf, len_out.value + 1, None)
            return buf.value.decode()

        child = _n.Handle(0)
        if lib.bison_get_object(h, k, ctypes.byref(child)) == _n.BISON_OK:
            return Dynamic(_handle=child.value) if child.value else None

        raise KeyError(f"Field '{name}' not found or has an unsupported type")

    def _get_at(self, index: int) -> Any:
        lib, h = self._lib, self._handle

        v_int = ctypes.c_int32(0)
        if lib.bison_get_int_at(h, index, ctypes.byref(v_int)) == _n.BISON_OK:
            return int(v_int.value)

        v_float = ctypes.c_float(0.0)
        if lib.bison_get_float_at(h, index, ctypes.byref(v_float)) == _n.BISON_OK:
            return float(v_float.value)

        len_out = ctypes.c_size_t(0)
        if lib.bison_get_string_at(h, index, None, 0, ctypes.byref(len_out)) == _n.BISON_OK:
            buf = ctypes.create_string_buffer(len_out.value + 1)
            lib.bison_get_string_at(h, index, buf, len_out.value + 1, None)
            return buf.value.decode()

        raise IndexError(f"Index {index} not found or has an unsupported type")

    # Note: no __contains__ is provided. The underlying C ABI has no
    # "field exists" query — bison_get_int() et al. lazily create a
    # zero-valued field on first access (matching dynamic::operator[]
    # semantics in C++), so a containment check would itself mutate the
    # object. Use field_attributes()/method_attributes() (which do fail
    # with BISON_ERR_NOT_FOUND) where an existence check is needed.

    def __delitem__(self, name: Any) -> None:
        raise TypeError("Dynamic fields cannot be deleted, only reassigned")

    # ── Array-like helpers ───────────────────────────────────────────────────

    def size(self) -> int:
        """Number of array-like (numeric-key) elements."""
        return self._lib.bison_size(self._handle)

    def __len__(self) -> int:
        return self.size()

    def __iter__(self) -> Iterator[Any]:
        for i in range(self.size()):
            yield self[i]

    # ── Methods ───────────────────────────────────────────────────────────────

    def add_method(self, name: str, fn: Callable[["Dynamic", "Dynamic", "Dynamic"], None]) -> None:
        """Register a Python callable as a named method.

        *fn* has signature ``fn(self: Dynamic, params: Dynamic, result: Dynamic)``
        and must populate *result* in place (it must not be released).
        """
        lib = self._lib

        def c_callback(self_h, params_h, result_h, _user) -> None:
            self_dyn = Dynamic(_handle=self_h, _owned=False)
            params_dyn = Dynamic(_handle=params_h, _owned=False)
            result_dyn = Dynamic(_handle=result_h, _owned=False)
            try:
                fn(self_dyn, params_dyn, result_dyn)
            except Exception:
                pass  # C ABI boundary: exceptions must not cross back into C++

        c_fn = _n.MethodFn(c_callback)
        self._callbacks.append(c_fn)  # keep alive for the object's lifetime
        _check(lib.bison_add_method(self._handle, key(name), c_fn, None, None), f"add_method({name!r})")

    def call(self, name: str, params: Optional["Dynamic"] = None) -> "Dynamic":
        """Invoke a named method on this object and return its result."""
        owns_params = params is None
        if params is None:
            params = Dynamic()
        result_h = _n.Handle(0)
        try:
            _check(
                self._lib.bison_call(self._handle, key(name), params._handle, ctypes.byref(result_h)),
                f"call({name!r})",
            )
        finally:
            if owns_params:
                params.release()
        return Dynamic(_handle=result_h.value)

    def __getattr__(self, name: str) -> Any:
        # Only reached when normal attribute lookup fails, so this never
        # shadows a real method/slot (get/set/call/release/...). Probes
        # bison_get_field_attributes / bison_get_method_attributes directly
        # (rather than through field_attributes()/method_attributes(), which
        # raise BisonError on a miss) since both are non-mutating existence
        # checks — unlike bison_get_int() et al., which lazily create a
        # field on a missing key (see the __contains__ note above).
        if name.startswith("_"):
            raise AttributeError(name)

        k = key(name)
        c = _n.CAttributes()
        if self._lib.bison_get_field_attributes(self._handle, k, ctypes.byref(c)) == _n.BISON_OK:
            return self[name]
        if self._lib.bison_get_method_attributes(self._handle, k, ctypes.byref(c)) == _n.BISON_OK:

            def bound_method(**kwargs: Any) -> "Dynamic":
                params = Dynamic()
                try:
                    for pk, pv in kwargs.items():
                        params[pk] = pv
                    return self.call(name, params)
                finally:
                    params.release()

            return bound_method

        raise AttributeError(name)

    def __setattr__(self, name: str, value: Any) -> None:
        # Slots (_handle, _lib, ...) are set directly; anything else is a
        # field write, e.g. ``obj.name = "Alice"`` == ``obj["name"] = ...``.
        if name.startswith("_"):
            object.__setattr__(self, name, value)
        else:
            self[name] = value

    # ── Field / method attributes ────────────────────────────────────────────

    def field_attributes(self, name: str) -> Attributes:
        c = _n.CAttributes()
        _check(
            self._lib.bison_get_field_attributes(self._handle, key(name), ctypes.byref(c)),
            f"field_attributes({name!r})",
        )
        return Attributes._from_c(c)

    def method_attributes(self, name: str) -> Attributes:
        c = _n.CAttributes()
        _check(
            self._lib.bison_get_method_attributes(self._handle, key(name), ctypes.byref(c)),
            f"method_attributes({name!r})",
        )
        return Attributes._from_c(c)

    # ── Field registration with metadata ────────────────────────────────────

    def add_field(self, name: str, value: Any, meta: Optional[Attributes] = None) -> None:
        """Declare a new field with optional documentation metadata.

        Unlike ``obj[name] = value`` this fails with :class:`BisonError`
        (``BISON_ERR_DUPLICATE``) if the field already exists.
        """
        lib, h, k, m = self._lib, self._handle, key(name), _meta_ptr(meta)
        if isinstance(value, bool):
            _check(lib.bison_add_field_bool(h, k, int(value), m), f"add_field_bool[{name}]")
        elif isinstance(value, int):
            _check(lib.bison_add_field_int(h, k, value, m), f"add_field_int[{name}]")
        elif isinstance(value, float):
            _check(lib.bison_add_field_float(h, k, value, m), f"add_field_float[{name}]")
        elif isinstance(value, str):
            _check(lib.bison_add_field_string(h, k, value.encode(), m), f"add_field_string[{name}]")
        else:
            raise TypeError(f"Unsupported value type for add_field: {type(value)}")

    # ── Serialization ────────────────────────────────────────────────────────

    def to_json(self, indent: int = 2) -> str:
        """Serialize to a JSON string. Pass ``indent=-1`` for compact output."""
        out = ctypes.c_char_p()
        _check(self._lib.bison_to_json(self._handle, indent, ctypes.byref(out)), "to_json")
        try:
            return out.value.decode()
        finally:
            self._lib.bison_free_string(out)

    def to_yaml(self) -> str:
        """Serialize to a YAML string."""
        out = ctypes.c_char_p()
        _check(self._lib.bison_to_yaml(self._handle, ctypes.byref(out)), "to_yaml")
        try:
            return out.value.decode()
        finally:
            self._lib.bison_free_string(out)

    def pretty(self, multiline: bool = True, indent: str = "  ") -> str:
        """Human-readable representation (via ``bison_print``)."""
        opts = _n.CPrintOptions(multiline=int(multiline), indent=indent.encode())
        out = ctypes.c_char_p()
        _check(self._lib.bison_print(self._handle, ctypes.byref(opts), ctypes.byref(out)), "pretty")
        try:
            return out.value.decode()
        finally:
            self._lib.bison_free_string(out)

    # ── Repr ──────────────────────────────────────────────────────────────────

    def __repr__(self) -> str:
        invalid = (self._owned and self._released) or not self._handle
        state = "released" if invalid else f"handle=0x{self._handle:x}"
        n = 0 if invalid else self.size()
        return f"Dynamic({state}, size={n})"

    def __str__(self) -> str:
        return self.pretty()


# ─── Factory / registry functions ───────────────────────────────────────────


def from_json(text: str) -> Dynamic:
    """Parse a JSON string and return the root object."""
    h = _n.get_lib().bison_from_json(text.encode())
    if not h:
        raise ValueError("from_json: invalid or unsupported JSON")
    return Dynamic(_handle=h)


def from_yaml(text: str) -> Dynamic:
    """Parse a YAML string and return the root object."""
    h = _n.get_lib().bison_from_yaml(text.encode())
    if not h:
        raise ValueError("from_yaml: invalid or unsupported YAML")
    return Dynamic(_handle=h)


# bison_add_class() copies field/method data out of *prototype* into the C++
# registry — it does not take ownership of the handle (see bison_c.h) — but
# a registered method only copies the raw C function pointer, i.e. the
# ctypes closure trampoline behind each MethodFn. That trampoline is only
# kept alive by Python references (Dynamic._callbacks); objects later
# instantiated from the class (bison_instantiate) call back into it directly.
# So every prototype ever registered must be kept alive for as long as its
# class stays in the registry, or the trampoline gets freed out from under
# the C++ side. This list mirrors the registry's own lifetime.
_registered_prototypes: list = []


def add_class(
    prototype: Dynamic, parent_name: str = "", ns_name: str = "", meta: Optional[Attributes] = None
) -> None:
    """Register *prototype* (whose ``__class`` field is already set) as a
    class in the global (or named) namespace registry."""
    ns_key = key(ns_name) if ns_name else 0
    parent_key = key(parent_name) if parent_name else 0
    _check(
        _n.get_lib().bison_add_class(ns_key, prototype._handle, parent_key, _meta_ptr(meta)),
        f"add_class({parent_name!r})",
    )
    _registered_prototypes.append(prototype)


def find_class(klass_name: str, ns_name: str = "") -> Optional[Dynamic]:
    """Look up a registered class prototype. Returns a non-owning view, or
    ``None`` if not found."""
    ns_key = key(ns_name) if ns_name else 0
    h = _n.get_lib().bison_find_class(ns_key, key(klass_name))
    return Dynamic(_handle=h, _owned=False) if h else None


def instantiate(klass_name: str, ns_name: str = "") -> Dynamic:
    """Create a new instance of a registered class (with inherited fields
    and methods)."""
    ns_key = key(ns_name) if ns_name else 0
    h = _n.get_lib().bison_instantiate(ns_key, key(klass_name))
    if not h:
        raise BisonError(_n.BISON_ERR_NOT_FOUND, f"instantiate({klass_name!r})")
    return Dynamic(_handle=h)


def clear_registry() -> None:
    """Remove all registered classes from the global registry."""
    _n.get_lib().bison_clear_registry()
    _registered_prototypes.clear()


def class_attributes(klass_name: str, ns_name: str = "") -> Attributes:
    """Read the class-level attribute metadata of a registered class."""
    ns_key = key(ns_name) if ns_name else 0
    c = _n.CAttributes()
    _check(
        _n.get_lib().bison_get_class_attributes(ns_key, key(klass_name), ctypes.byref(c)),
        f"class_attributes({klass_name!r})",
    )
    return Attributes._from_c(c)

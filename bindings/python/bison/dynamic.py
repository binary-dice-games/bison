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
from functools import lru_cache
from typing import Any, Callable, Iterator, Optional, Sequence, Union

from . import _native as _n

__all__ = [
    "BisonError",
    "Attributes",
    "Dynamic",
    "key",
    "from_json",
    "from_yaml",
    "deserialize",
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


@lru_cache(maxsize=4096)
def key(name: str) -> int:
    """Return the 32-bit FNV-1a hash of *name* (same as ``"name"_key`` in C++).

    Unlike the C++ side (where ``"name"_key`` is a compile-time constant),
    Python has no way to hash a field/method name at "compile" time, so
    every ``Dynamic`` field/method access funnels through this function at
    run time. Results are memoized: field and method names are drawn from
    a small, static schema-defined set reused across many calls, and the
    hash is a pure function of the string, so caching avoids repeating the
    string encode + native call (which also pays ctypes' per-call GIL
    release/reacquire) for names already seen. Bounded (rather than
    unbounded) so pathological callers hashing high-cardinality or
    externally-derived strings can't grow the cache without bound.
    """
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


# ─── Vector field helpers ────────────────────────────────────────────────────
#
# A Python list/tuple has no fixed element type the way bison::key_t / int32_t
# / float do, so -- unlike the scalar dispatches above, which switch on
# isinstance(value, ...) directly -- writing a vector field needs to inspect
# the sequence's first element to decide which of bison_{add_field_vector,
# set_vector}_{bool,int,float}() to call. bytes/bytearray map directly to
# vector<uint8_t> and need no such inspection.

_NOT_A_VECTOR = object()  # sentinel: "none of the four vector getters matched"


def _vector_kind(values: Sequence[Any]) -> str:
    """Classify a list/tuple as "bool", "int", or "float" for vector dispatch.

    An empty sequence carries no element-type information at all; it
    defaults to "int" (vector<int32_t>), the same default plain Python
    ``int`` gets elsewhere in this module.
    """
    if len(values) == 0:
        return "int"
    first = values[0]
    if isinstance(first, bool):
        return "bool"
    if isinstance(first, int):
        return "int"
    if isinstance(first, float):
        return "float"
    raise TypeError(f"Unsupported vector element type: {type(first)}")


def _add_field_vector(lib, h, k: int, name: Any, values: Sequence[Any], meta_ptr) -> None:
    kind = _vector_kind(values)
    count = len(values)
    if kind == "bool":
        arr = (ctypes.c_int * count)(*[int(v) for v in values]) if values else None
        _check(lib.bison_add_field_vector_bool(h, k, arr, count, meta_ptr), f"add_field_vector_bool[{name}]")
    elif kind == "int":
        arr = (ctypes.c_int32 * count)(*values) if values else None
        _check(lib.bison_add_field_vector_int(h, k, arr, count, meta_ptr), f"add_field_vector_int[{name}]")
    else:
        arr = (ctypes.c_float * count)(*values) if values else None
        _check(lib.bison_add_field_vector_float(h, k, arr, count, meta_ptr), f"add_field_vector_float[{name}]")


def _set_vector_field(lib, h, k: int, name: Any, values: Sequence[Any]) -> None:
    kind = _vector_kind(values)
    if kind == "bool":
        arr = (ctypes.c_int * len(values))(*[int(v) for v in values]) if values else None
        _check(lib.bison_set_vector_bool(h, k, arr, len(values)), f"set_vector_bool[{name}]")
    elif kind == "int":
        arr = (ctypes.c_int32 * len(values))(*values) if values else None
        _check(lib.bison_set_vector_int(h, k, arr, len(values)), f"set_vector_int[{name}]")
    else:
        arr = (ctypes.c_float * len(values))(*values) if values else None
        _check(lib.bison_set_vector_float(h, k, arr, len(values)), f"set_vector_float[{name}]")


def _set_object_array_field(lib, h, k: int, name: Any, values: Sequence[Any]) -> None:
    """Build @p values (a list/tuple of ``dict``/:class:`Dynamic`) as a bison
    array-of-objects field: a scratch :class:`Dynamic` with sequential
    integer keys ``0..len(values)-1``, each holding one converted element,
    assigned to field @p k the same way a single nested :class:`Dynamic`
    value already is (`__setitem__`'s `Dynamic` branch). A plain ``dict``
    element is converted via a fresh :class:`Dynamic` populated field-by-field
    through `__setitem__` itself, so a `dict` field whose own value is a
    list of dicts recurses back into this same function -- nesting "just
    works" without a separate recursive-conversion helper.
    """
    arr = Dynamic()
    try:
        for i, item in enumerate(values):
            if isinstance(item, Dynamic):
                arr[i] = item
            elif isinstance(item, dict):
                child = Dynamic()
                try:
                    for ck, cv in item.items():
                        child[ck] = cv
                    arr[i] = child
                finally:
                    child.release()
            else:
                raise TypeError(f"Unsupported object-array element type: {type(item)}")
        _check(lib.bison_set_object(h, k, arr._handle), f"set_object[{name}]")
    finally:
        arr.release()


def _get_vector_field(lib, h, k: int) -> Any:
    """Try each vector getter in turn. Returns :data:`_NOT_A_VECTOR` if the
    field holds none of them (see the callers' cascade-ordering notes)."""
    len_out = ctypes.c_size_t(0)

    if lib.bison_get_vector_int(h, k, None, 0, ctypes.byref(len_out)) == _n.BISON_OK:
        arr = (ctypes.c_int32 * len_out.value)()
        if len_out.value:
            lib.bison_get_vector_int(h, k, arr, len_out.value, None)
        return list(arr)

    if lib.bison_get_vector_float(h, k, None, 0, ctypes.byref(len_out)) == _n.BISON_OK:
        arr = (ctypes.c_float * len_out.value)()
        if len_out.value:
            lib.bison_get_vector_float(h, k, arr, len_out.value, None)
        return list(arr)

    if lib.bison_get_vector_bool(h, k, None, 0, ctypes.byref(len_out)) == _n.BISON_OK:
        arr = (ctypes.c_int * len_out.value)()
        if len_out.value:
            lib.bison_get_vector_bool(h, k, arr, len_out.value, None)
        return [bool(v) for v in arr]

    if lib.bison_get_vector_bytes(h, k, None, 0, ctypes.byref(len_out)) == _n.BISON_OK:
        buf = (ctypes.c_uint8 * len_out.value)()
        if len_out.value:
            lib.bison_get_vector_bytes(h, k, buf, len_out.value, None)
        return bytes(buf)

    return _NOT_A_VECTOR


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
            # bool is checked before int (bool is a subclass of int in
            # Python) so it round-trips as a real bison bool field via
            # bison_set_bool_at() -- not silently coerced to int32, the same
            # distinction bison_set_bool()/bison_set_int() already make for
            # named fields.
            if isinstance(value, bool):
                _check(lib.bison_set_bool_at(h, name, int(value)), f"set_bool_at[{name}]")
            elif isinstance(value, int):
                _check(lib.bison_set_int_at(h, name, value), f"set_int_at[{name}]")
            elif isinstance(value, float):
                _check(lib.bison_set_float_at(h, name, value), f"set_float_at[{name}]")
            elif isinstance(value, str):
                _check(lib.bison_set_string_at(h, name, value.encode()), f"set_string_at[{name}]")
            elif isinstance(value, Dynamic):
                _check(lib.bison_set_object_at(h, name, value._handle), f"set_object_at[{name}]")
            elif value is None:
                _check(lib.bison_set_object_at(h, name, None), f"set_object_at_null[{name}]")
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
        elif isinstance(value, (bytes, bytearray)):
            arr = (ctypes.c_uint8 * len(value))(*value) if value else None
            _check(lib.bison_set_vector_bytes(h, k, arr, len(value)), f"set_vector_bytes[{name}]")
        elif isinstance(value, (list, tuple)):
            # A list/tuple of dict/Dynamic elements (e.g. a JSON-array-of-
            # objects param, one dict per row) builds an array-of-objects
            # field instead of the scalar-only vector path -- see
            # _set_object_array_field()'s doc comment. Classified the same
            # way _vector_kind() classifies scalars: by its first element.
            if value and isinstance(value[0], (dict, Dynamic)):
                _set_object_array_field(lib, h, k, name, value)
            else:
                _set_vector_field(lib, h, k, name, value)
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

        # bison_get_key() is checked last, matching every other cascade step:
        # it only ever succeeds here for a field already holding a key_t
        # value (bison_get_int() above would already have auto-vivified a
        # genuinely absent field as int32 zero -- see set_key()'s docstring).
        v_key = _n.Hash(0)
        if lib.bison_get_key(h, k, ctypes.byref(v_key)) == _n.BISON_OK:
            return int(v_key.value)

        # Vector-typed fields are tried last of all, for the same reason
        # bison_get_key() is: a field that was never explicitly set as a
        # vector would already have been claimed (and auto-vivified) by
        # bison_get_int() above, so these four only ever succeed for a
        # field actually holding that vector variant.
        vector = _get_vector_field(lib, h, k)
        if vector is not _NOT_A_VECTOR:
            return vector

        raise KeyError(f"Field '{name}' not found or has an unsupported type")

    def _get_at(self, index: int) -> Any:
        lib, h = self._lib, self._handle

        v_int = ctypes.c_int32(0)
        if lib.bison_get_int_at(h, index, ctypes.byref(v_int)) == _n.BISON_OK:
            return int(v_int.value)

        v_float = ctypes.c_float(0.0)
        if lib.bison_get_float_at(h, index, ctypes.byref(v_float)) == _n.BISON_OK:
            return float(v_float.value)

        v_bool = ctypes.c_int(0)
        if lib.bison_get_bool_at(h, index, ctypes.byref(v_bool)) == _n.BISON_OK:
            return bool(v_bool.value)

        len_out = ctypes.c_size_t(0)
        if lib.bison_get_string_at(h, index, None, 0, ctypes.byref(len_out)) == _n.BISON_OK:
            buf = ctypes.create_string_buffer(len_out.value + 1)
            lib.bison_get_string_at(h, index, buf, len_out.value + 1, None)
            return buf.value.decode()

        child = _n.Handle(0)
        if lib.bison_get_object_at(h, index, ctypes.byref(child)) == _n.BISON_OK:
            return Dynamic(_handle=child.value) if child.value else None

        # See __getitem__'s identical note on why bison_get_key() is tried
        # last: it only succeeds for an index already holding a key_t value.
        v_key = _n.Hash(0)
        if lib.bison_get_key_at(h, index, ctypes.byref(v_key)) == _n.BISON_OK:
            return int(v_key.value)

        raise IndexError(f"Index {index} not found or has an unsupported type")

    # Note: no __contains__ is provided. The underlying C ABI has no
    # "field exists" query — bison_get_int() et al. lazily create a
    # zero-valued field on first access (matching dynamic::operator[]
    # semantics in C++), so a containment check would itself mutate the
    # object. Use field_attributes()/method_attributes() (which do fail
    # with BISON_ERR_NOT_FOUND) where an existence check is needed.

    def __delitem__(self, name: Any) -> None:
        raise TypeError("Dynamic fields cannot be deleted, only reassigned")

    # ── bison::key_t-typed field access ─────────────────────────────────────
    #
    # A field the C++ side declares as `bison::key_t` (e.g. an object's "id",
    # or an enum-like selector) is a distinct bison field-variant type from
    # `int32_t` -- reading one is handled by __getitem__'s cascade above (its
    # final fallback, after int/float/bool/string/object all fail by type
    # mismatch), but writing one needs an explicit call: a plain Python `int`
    # handed to __setitem__ is always written as int32 (there is no way to
    # tell "this int should become a key_t field" from "this int should
    # become an int32 field" without one).

    def set_key(self, name: str, value: Union[int, str]) -> None:
        """Set a ``bison::key_t``-valued field (e.g. an object's ``"id"``).

        *value* is either an already-hashed int (e.g. from :func:`key`) or a
        name string, hashed the same way ``"name"_key`` would in C++.
        """
        v = value if isinstance(value, int) else key(value)
        _check(self._lib.bison_set_key(self._handle, key(name), v), f"set_key[{name}]")

    def set_key_at(self, index: int, value: Union[int, str]) -> None:
        """Set a ``bison::key_t``-valued field by numeric index -- the
        indexed counterpart to :meth:`set_key`, for the same reason a bare
        ``obj[index] = value`` int assignment can't dispatch to this."""
        v = value if isinstance(value, int) else key(value)
        _check(self._lib.bison_set_key_at(self._handle, index, v), f"set_key_at[{index}]")

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
        elif isinstance(value, (bytes, bytearray)):
            arr = (ctypes.c_uint8 * len(value))(*value) if value else None
            _check(lib.bison_add_field_vector_bytes(h, k, arr, len(value), m), f"add_field_vector_bytes[{name}]")
        elif isinstance(value, (list, tuple)):
            _add_field_vector(lib, h, k, name, value, m)
        else:
            raise TypeError(f"Unsupported value type for add_field: {type(value)}")

    def add_field_key(self, name: str, value: Union[int, str], meta: Optional[Attributes] = None) -> None:
        """Declare a new ``bison::key_t``-valued field -- the ``add_field()``
        counterpart to :meth:`set_key`, for the same reason ``add_field()``
        itself can't dispatch to this from a plain Python ``int``.

        Fails with :class:`BisonError` (``BISON_ERR_DUPLICATE``) if the
        field already exists. *value* is either an already-hashed int or a
        name string, hashed the same way :meth:`set_key` does.
        """
        v = value if isinstance(value, int) else key(value)
        _check(
            self._lib.bison_add_field_key(self._handle, key(name), v, _meta_ptr(meta)), f"add_field_key[{name}]"
        )

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

    def serialize(self) -> bytes:
        """Serialize to the compact binary wire format (see ``FORMAT.md``) --
        the counterpart to module-level :func:`deserialize`. Field keys are
        encoded as their raw hash, so (unlike :meth:`to_json`/:meth:`to_yaml`)
        this format is self-contained and needs no key-name map to round-trip.
        """
        out = ctypes.POINTER(ctypes.c_uint8)()
        out_len = ctypes.c_size_t(0)
        _check(self._lib.bison_serialize(self._handle, ctypes.byref(out), ctypes.byref(out_len)), "serialize")
        try:
            return ctypes.string_at(out, out_len.value)
        finally:
            self._lib.bison_free_buffer(out)

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


def deserialize(data: bytes) -> Dynamic:
    """Deserialize a buffer produced by :meth:`Dynamic.serialize`."""
    lib = _n.get_lib()
    buf = (ctypes.c_uint8 * len(data)).from_buffer_copy(data) if data else None
    h = _n.Handle(0)
    _check(lib.bison_deserialize(buf, len(data), ctypes.byref(h)), "deserialize")
    return Dynamic(_handle=h.value)


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

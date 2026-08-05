"""Pythonic RAII wrapper around ``rmi_c.h`` — the Bison RMI C ABI.

Provides :class:`Client` / :class:`Server` (context managers over
``rmi_client_handle`` / ``rmi_server_handle``) and :class:`Proxy`, a
dict- and attribute-style handle to a remote (or in-process standalone)
object: ``proxy["field"]``, ``proxy["field"] = value``, and
``proxy.method_name(**kwargs)`` all forward to the underlying C ABI calls.
"""

import ctypes
from contextlib import contextmanager
from typing import Any, Callable, Optional

from . import _native as _n
from .dynamic import Dynamic, key

__all__ = ["RmiError", "Future", "Proxy", "Client", "Server"]


class RmiError(RuntimeError):
    """Raised when an ``rmi_*`` C API call returns a non-zero error code."""

    def __init__(self, code: int, context: str = ""):
        msg = _n.RMI_ERROR_MESSAGES.get(code, f"Unknown error {code}")
        super().__init__(f"{context}: {msg}" if context else msg)
        self.code = code


def _check(rc: int, context: str = "") -> None:
    if rc != _n.RMI_OK:
        raise RmiError(rc, context)


@contextmanager
def _as_params(params: Any):
    """Yield a raw ``bison_handle`` (or ``None``) for *params*.

    Accepts ``dict``, ``Dynamic``, or ``None``. A ``dict`` is converted into
    a scratch :class:`Dynamic` that is released on exit; a caller-owned
    :class:`Dynamic` is passed through untouched (its lifetime remains the
    caller's responsibility).
    """
    if params is None:
        yield None
    elif isinstance(params, Dynamic):
        yield params._handle
    elif isinstance(params, dict):
        d = Dynamic()
        try:
            for k, v in params.items():
                d[k] = v
            yield d._handle
        finally:
            d.release()
    else:
        raise TypeError(f"params must be a dict, Dynamic, or None, not {type(params)}")


class Future:
    """RAII wrapper around an ``rmi_future_handle``.

    Consumed exactly once via :meth:`get_dynamic` or :meth:`get_proxy`, or
    discarded with :meth:`release` / by leaving a ``with`` block.
    """

    __slots__ = ("_lib", "_handle")

    def __init__(self, handle: int):
        self._lib = _n.get_lib()
        self._handle = _n.FutureHandle(handle)

    def __enter__(self) -> "Future":
        return self

    def __exit__(self, *_exc) -> None:
        self.release()

    def wait(self, timeout_ms: int = -1) -> None:
        """Block until the operation completes (does not consume the future)."""
        _check(self._lib.rmi_future_wait(self._handle, timeout_ms), "future.wait")

    def get_dynamic(self) -> Dynamic:
        """Consume the future and return its :class:`Dynamic` result."""
        out = _n.Handle(0)
        _check(self._lib.rmi_future_get_dynamic(ctypes.byref(self._handle), ctypes.byref(out)), "future.get_dynamic")
        return Dynamic(_handle=out.value)

    def get_proxy(self) -> "Proxy":
        """Consume the future and return its :class:`Proxy` result."""
        out = _n.ProxyHandle(0)
        _check(self._lib.rmi_future_get_proxy(ctypes.byref(self._handle), ctypes.byref(out)), "future.get_proxy")
        return Proxy(out.value)

    def release(self) -> None:
        if self._handle:
            self._lib.rmi_future_release(self._handle)
            self._handle = _n.FutureHandle(0)

    def __del__(self):
        try:
            self.release()
        except Exception:
            pass


class Proxy:
    """A live handle to a remote (or in-process standalone) object.

    ``proxy["field"]`` / ``proxy["field"] = value`` project/patch a single
    field; ``proxy.some_method(a=1, b=2)`` invokes a remote method by name.
    """

    __slots__ = ("_lib", "_handle", "_callbacks")

    def __init__(self, handle: int):
        self._lib = _n.get_lib()
        self._handle = _n.ProxyHandle(handle)
        self._callbacks: list = []

    def __enter__(self) -> "Proxy":
        return self

    def __exit__(self, *_exc) -> None:
        self.release()

    def release(self) -> None:
        """Destroy the remote object and release this proxy."""
        if self._handle:
            self._lib.rmi_proxy_release(self._handle)
            self._handle = _n.ProxyHandle(0)

    def __del__(self):
        try:
            self.release()
        except Exception:
            pass

    # ── Remote field access ──────────────────────────────────────────────────

    def get(self, projection: Optional[dict] = None, timeout_ms: int = -1) -> Dynamic:
        """Fetch a full snapshot, or a projected subset of fields."""
        out = _n.Handle(0)
        with _as_params(projection) as ph:
            _check(self._lib.rmi_proxy_get(self._handle, ph, ctypes.byref(out), timeout_ms), "proxy.get")
        return Dynamic(_handle=out.value)

    def set(self, fields: dict, timeout_ms: int = -1) -> None:
        """Apply a partial field update without resetting unspecified fields."""
        with _as_params(fields) as ph:
            _check(self._lib.rmi_proxy_set(self._handle, ph, timeout_ms), "proxy.set")

    def clear(self, timeout_ms: int = -1) -> None:
        """Reset explicitly-set fields back to prototype/inherited defaults."""
        _check(self._lib.rmi_proxy_clear(self._handle, timeout_ms), "proxy.clear")

    def __getitem__(self, name: str) -> Any:
        return self.get({name: True})[name]

    def __setitem__(self, name: str, value: Any) -> None:
        self.set({name: value})

    # ── Remote method calls ──────────────────────────────────────────────────

    def call(self, name: str, params: Any = None, timeout_ms: int = -1) -> Dynamic:
        """Invoke a named remote method with *params* (dict or Dynamic)."""
        out = _n.Handle(0)
        with _as_params(params) as ph:
            _check(
                self._lib.rmi_proxy_call(self._handle, key(name), ph, ctypes.byref(out), timeout_ms),
                f"proxy.call({name!r})",
            )
        return Dynamic(_handle=out.value)

    def __getattr__(self, name: str) -> Callable[..., Dynamic]:
        # Only reached when normal attribute lookup fails, so this never
        # shadows a real method/slot (get/set/clear/call/release/...).
        if name.startswith("_"):
            raise AttributeError(name)

        def bound_method(**kwargs: Any) -> Dynamic:
            return self.call(name, kwargs)

        return bound_method

    # ── Events ────────────────────────────────────────────────────────────────

    def on_event(self, name: str, handler: Callable[[Dynamic], None]) -> None:
        """Subscribe to a server-initiated event on this object."""

        def c_callback(params_h, _user) -> None:
            try:
                handler(Dynamic(_handle=params_h, _owned=False))
            except Exception:
                pass

        c_fn = _n.ProxyEventFn(c_callback)
        self._callbacks.append(c_fn)
        _check(self._lib.rmi_proxy_on_event(self._handle, key(name), c_fn, None), f"on_event({name!r})")


class Client:
    """RAII wrapper around an ``rmi_client_handle``.

    Construct via :meth:`tcp`, :meth:`pipe`, :meth:`term`, or
    :meth:`standalone`; use as a context manager to auto-connect/disconnect.
    """

    __slots__ = ("_lib", "_handle", "_connected")

    def __init__(self, handle: int):
        self._lib = _n.get_lib()
        self._handle = _n.ClientHandle(handle)
        self._connected = False

    @classmethod
    def tcp(cls, host: str, port: int) -> "Client":
        h = _n.get_lib().rmi_client_tcp_create(host.encode(), port)
        if not h:
            raise MemoryError("rmi_client_tcp_create failed")
        return cls(h)

    @classmethod
    def pipe(cls, path: str) -> "Client":
        h = _n.get_lib().rmi_client_pipe_create(path.encode())
        if not h:
            raise MemoryError("rmi_client_pipe_create failed")
        return cls(h)

    @classmethod
    def term(cls) -> "Client":
        h = _n.get_lib().rmi_client_term_create()
        if not h:
            raise MemoryError("rmi_client_term_create failed")
        return cls(h)

    @classmethod
    def standalone(cls) -> "Client":
        """In-process client dispatching directly to the local class registry."""
        h = _n.get_lib().rmi_standalone_create()
        if not h:
            raise MemoryError("rmi_standalone_create failed")
        return cls(h)

    def connect(self, params: Any = None) -> "Client":
        with _as_params(params) as ph:
            _check(self._lib.rmi_client_connect(self._handle, ph), "connect")
        self._connected = True
        return self

    def disconnect(self) -> None:
        if self._connected:
            _check(self._lib.rmi_client_disconnect(self._handle), "disconnect")
            self._connected = False

    def release(self) -> None:
        if self._handle:
            self._lib.rmi_client_release(self._handle)
            self._handle = _n.ClientHandle(0)

    def __enter__(self) -> "Client":
        if not self._connected:
            self.connect()
        return self

    def __exit__(self, *_exc) -> None:
        self.disconnect()
        self.release()

    def __del__(self):
        try:
            self.release()
        except Exception:
            pass

    def instantiate(self, klass_name: str, ns_name: str = "", params: Any = None) -> Proxy:
        """Instantiate a remote object and return a :class:`Proxy` to it."""
        ns_key = key(ns_name) if ns_name else 0
        out = _n.ProxyHandle(0)
        with _as_params(params) as ph:
            _check(
                self._lib.rmi_client_instantiate(self._handle, ns_key, key(klass_name), ph, ctypes.byref(out)),
                f"instantiate({klass_name!r})",
            )
        return Proxy(out.value)

    def instantiate_async(self, klass_name: str, ns_name: str = "", params: Any = None) -> Future:
        ns_key = key(ns_name) if ns_name else 0
        out = _n.FutureHandle(0)
        with _as_params(params) as ph:
            _check(
                self._lib.rmi_client_instantiate_async(self._handle, ns_key, key(klass_name), ph, ctypes.byref(out)),
                f"instantiate_async({klass_name!r})",
            )
        return Future(out.value)

    def describe(self, klass_name: str = "", ns_name: str = "") -> Dynamic:
        """Fetch class metadata. Empty *klass_name* queries all metadata."""
        ns_key = key(ns_name) if ns_name else 0
        klass_key = key(klass_name) if klass_name else 0
        out = _n.Handle(0)
        _check(self._lib.rmi_client_describe(self._handle, ns_key, klass_key, ctypes.byref(out)), "describe")
        return Dynamic(_handle=out.value)

    def describe_async(self, klass_name: str = "", ns_name: str = "") -> Future:
        ns_key = key(ns_name) if ns_name else 0
        klass_key = key(klass_name) if klass_name else 0
        out = _n.FutureHandle(0)
        _check(
            self._lib.rmi_client_describe_async(self._handle, ns_key, klass_key, ctypes.byref(out)), "describe_async"
        )
        return Future(out.value)


class Server:
    """RAII wrapper around an ``rmi_server_handle``.

    Construct via :meth:`tcp`, :meth:`pipe`, or :meth:`term`; use as a
    context manager to auto-listen/stop.
    """

    __slots__ = ("_lib", "_handle", "_listening", "_callbacks")

    def __init__(self, handle: int):
        self._lib = _n.get_lib()
        self._handle = _n.ServerHandle(handle)
        self._listening = False
        self._callbacks: list = []

    @classmethod
    def tcp(cls, host: str, port: int) -> "Server":
        h = _n.get_lib().rmi_server_tcp_create(host.encode(), port)
        if not h:
            raise MemoryError("rmi_server_tcp_create failed")
        return cls(h)

    @classmethod
    def pipe(cls, path: str) -> "Server":
        h = _n.get_lib().rmi_server_pipe_create(path.encode())
        if not h:
            raise MemoryError("rmi_server_pipe_create failed")
        return cls(h)

    @classmethod
    def term(cls, cmd: Optional[str] = None) -> "Server":
        h = _n.get_lib().rmi_server_term_create(cmd.encode() if cmd else None)
        if not h:
            raise MemoryError("rmi_server_term_create failed")
        return cls(h)

    def listen(self, params: Any = None, auth: Optional[Callable[[Dynamic], "tuple[bool, str]"]] = None) -> "Server":
        """Start accepting client connections.

        *auth*, if given, is evaluated once per incoming connection for as
        long as the server keeps listening -- it can only be set here, not
        changed afterward. It receives the client's ``OP_CONNECT`` payload
        as a :class:`Dynamic` and returns ``(accepted, identity)``: whether
        to accept the connection, and (if accepted) an identity string.
        Identity strings longer than 255 UTF-8 bytes are truncated (the
        C ABI uses a fixed 256-byte buffer).
        """
        c_fn = _n.AuthFn()  # NULL function pointer unless *auth* is given
        if auth is not None:

            def c_callback(payload_h, identity_buf, identity_buf_len, _user) -> bool:
                try:
                    accepted, identity = auth(Dynamic(_handle=payload_h, _owned=False))
                except Exception:
                    return False
                if accepted and identity:
                    encoded = identity.encode("utf-8")[: identity_buf_len - 1]
                    ctypes.memmove(identity_buf, encoded + b"\0", len(encoded) + 1)
                return accepted

            c_fn = _n.AuthFn(c_callback)
            self._callbacks.append(c_fn)

        with _as_params(params) as ph:
            _check(self._lib.rmi_server_listen(self._handle, ph, c_fn, None), "listen")
        self._listening = True
        return self

    def stop(self) -> None:
        if self._listening:
            self._lib.rmi_server_stop(self._handle)
            self._listening = False

    def release(self) -> None:
        if self._handle:
            self._lib.rmi_server_release(self._handle)
            self._handle = _n.ServerHandle(0)

    def __enter__(self) -> "Server":
        if not self._listening:
            self.listen()
        return self

    def __exit__(self, *_exc) -> None:
        self.stop()
        self.release()

    def __del__(self):
        try:
            self.release()
        except Exception:
            pass

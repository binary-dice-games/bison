"""ctypes bindings for the ``bison_abi`` shared library.

Loads ``libbison_abi.so`` / ``.dylib`` / ``bison_abi.dll`` (built from
``bison_c.h`` and ``rmi_c.h``) and binds every exported C function with its
proper argument/return types.  This module is internal; use
:mod:`bison.dynamic` and :mod:`bison.rmi` for the public API.
"""

import ctypes
import ctypes.util
import os
import threading
from typing import Optional

# ─── Shared C type aliases ─────────────────────────────────────────────────

Handle = ctypes.c_void_p  # bison_handle
Hash = ctypes.c_uint32  # bison_hash
Error = ctypes.c_int  # bison_error / rmi_error

ClientHandle = ctypes.c_void_p  # rmi_client_handle
ServerHandle = ctypes.c_void_p  # rmi_server_handle
ProxyHandle = ctypes.c_void_p  # rmi_proxy_handle
FutureHandle = ctypes.c_void_p  # rmi_future_handle

# ─── bison_error codes ──────────────────────────────────────────────────────

BISON_OK = 0
BISON_ERR_NULL = -1
BISON_ERR_TYPE = -2
BISON_ERR_NOT_FOUND = -3
BISON_ERR_DUPLICATE = -4
BISON_ERR_EXCEPTION = -5
BISON_ERR_PARSE = -6

BISON_ERROR_MESSAGES = {
    BISON_ERR_NULL: "Null handle or pointer",
    BISON_ERR_TYPE: "Field type mismatch",
    BISON_ERR_NOT_FOUND: "Method or field not found",
    BISON_ERR_DUPLICATE: "Duplicate class or method",
    BISON_ERR_EXCEPTION: "Internal C++ exception",
    BISON_ERR_PARSE: "Parse error (JSON / YAML / binary buffer)",
}

# ─── rmi_error codes ────────────────────────────────────────────────────────

RMI_OK = 0
RMI_ERR_NULL = -1
RMI_ERR_INVALID_STATE = -2
RMI_ERR_TIMEOUT = -3
RMI_ERR_REMOTE_EXCEPTION = -4
RMI_ERR_TRANSPORT = -5
RMI_ERR_EXCEPTION = -6

RMI_ERROR_MESSAGES = {
    RMI_ERR_NULL: "Null handle or pointer",
    RMI_ERR_INVALID_STATE: "Operation invalid for current state",
    RMI_ERR_TIMEOUT: "Request timed out",
    RMI_ERR_REMOTE_EXCEPTION: "Server raised an exception",
    RMI_ERR_TRANSPORT: "Transport error",
    RMI_ERR_EXCEPTION: "Internal C++ exception",
}

# ─── Callback types ─────────────────────────────────────────────────────────

# void (*bison_method_fn)(bison_handle self, bison_handle params, bison_handle result, void* user)
MethodFn = ctypes.CFUNCTYPE(None, Handle, Handle, Handle, ctypes.c_void_p)

# void (*rmi_proxy_event_fn)(bison_handle params, void* user)
ProxyEventFn = ctypes.CFUNCTYPE(None, Handle, ctypes.c_void_p)

# bool (*rmi_auth_fn)(bison_handle payload, char* identity_buf, size_t identity_buf_len, void* user)
# identity_buf is declared c_void_p (not c_char_p) so the callback receives a
# writable address rather than an immutable copied `bytes`.
AuthFn = ctypes.CFUNCTYPE(ctypes.c_bool, Handle, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p)


# ─── Struct types ───────────────────────────────────────────────────────────


class CAttributes(ctypes.Structure):
    """Mirrors ``bison_attributes``."""

    _fields_ = [
        ("display_name", ctypes.c_char_p),
        ("description", ctypes.c_char_p),
        ("category", ctypes.c_char_p),
        ("obsolete", ctypes.c_int),
        ("obsolete_message", ctypes.c_char_p),
        ("required", ctypes.c_int),
    ]


class CPrintOptions(ctypes.Structure):
    """Mirrors ``bison_print_options``."""

    _fields_ = [
        ("multiline", ctypes.c_int),
        ("indent", ctypes.c_char_p),
    ]


# ─── Library loading ────────────────────────────────────────────────────────


def _find_library() -> str:
    """Locate ``libbison_abi`` via ``BISON_LIB``, the ``build/`` dir, or the
    system library search path."""
    env_path = os.environ.get("BISON_LIB")
    if env_path:
        return env_path

    # Layout: <repo>/bindings/python/bison/_native.py -> <repo>/build/...
    here = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.dirname(os.path.dirname(os.path.dirname(here)))
    candidates = [
        os.path.join(repo_root, "build", "libbison_abi.so"),
        os.path.join(repo_root, "build", "libbison_abi.dylib"),
        os.path.join(repo_root, "build", "Release", "bison_abi.dll"),
        os.path.join(repo_root, "build", "Debug", "bison_abi.dll"),
    ]
    for path in candidates:
        if os.path.isfile(path):
            return path

    found = ctypes.util.find_library("bison_abi")
    if found:
        return found

    raise OSError(
        "libbison_abi not found. Build it first "
        "(cmake --build build --target bison_abi) and/or set the BISON_LIB "
        "environment variable to the full path of libbison_abi.so/.dylib/bison_abi.dll."
    )


_lib_lock = threading.Lock()
_lib: Optional[ctypes.CDLL] = None


def get_lib() -> ctypes.CDLL:
    """Return the loaded ``bison_abi`` library (singleton, thread-safe)."""
    global _lib
    if _lib is None:
        with _lib_lock:
            if _lib is None:
                lib = ctypes.CDLL(_find_library())
                _setup_signatures(lib)
                _lib = lib
    return _lib


def _setup_signatures(lib: ctypes.CDLL) -> None:
    P = ctypes.POINTER

    # ── bison_c.h: lifecycle ────────────────────────────────────────────────
    lib.bison_create.restype = Handle
    lib.bison_create.argtypes = [Hash]

    lib.bison_instantiate.restype = Handle
    lib.bison_instantiate.argtypes = [Hash, Hash]

    lib.bison_add_ref.restype = Handle
    lib.bison_add_ref.argtypes = [Handle]

    lib.bison_release.restype = None
    lib.bison_release.argtypes = [Handle]

    lib.bison_clone.restype = Handle
    lib.bison_clone.argtypes = [Handle]

    # ── Import / export ─────────────────────────────────────────────────────
    lib.bison_from_json.restype = Handle
    lib.bison_from_json.argtypes = [ctypes.c_char_p]

    lib.bison_from_yaml.restype = Handle
    lib.bison_from_yaml.argtypes = [ctypes.c_char_p]

    lib.bison_to_json.restype = Error
    lib.bison_to_json.argtypes = [Handle, ctypes.c_int, P(ctypes.c_char_p)]

    lib.bison_to_yaml.restype = Error
    lib.bison_to_yaml.argtypes = [Handle, P(ctypes.c_char_p)]

    lib.bison_print.restype = Error
    lib.bison_print.argtypes = [Handle, P(CPrintOptions), P(ctypes.c_char_p)]

    lib.bison_free_string.restype = None
    lib.bison_free_string.argtypes = [ctypes.c_char_p]

    # ── Class registry ──────────────────────────────────────────────────────
    lib.bison_add_class.restype = Error
    lib.bison_add_class.argtypes = [Hash, Handle, Hash, P(CAttributes)]

    lib.bison_find_class.restype = Handle
    lib.bison_find_class.argtypes = [Hash, Hash]

    lib.bison_clear_registry.restype = None
    lib.bison_clear_registry.argtypes = []

    lib.bison_get_class_attributes.restype = Error
    lib.bison_get_class_attributes.argtypes = [Hash, Hash, P(CAttributes)]

    lib.bison_get_field_attributes.restype = Error
    lib.bison_get_field_attributes.argtypes = [Handle, Hash, P(CAttributes)]

    lib.bison_get_method_attributes.restype = Error
    lib.bison_get_method_attributes.argtypes = [Handle, Hash, P(CAttributes)]

    # ── Scalar setters (named) ──────────────────────────────────────────────
    lib.bison_set_int.restype = Error
    lib.bison_set_int.argtypes = [Handle, Hash, ctypes.c_int32]
    lib.bison_set_float.restype = Error
    lib.bison_set_float.argtypes = [Handle, Hash, ctypes.c_float]
    lib.bison_set_bool.restype = Error
    lib.bison_set_bool.argtypes = [Handle, Hash, ctypes.c_int]
    lib.bison_set_string.restype = Error
    lib.bison_set_string.argtypes = [Handle, Hash, ctypes.c_char_p]
    lib.bison_set_object.restype = Error
    lib.bison_set_object.argtypes = [Handle, Hash, Handle]
    lib.bison_set_key.restype = Error
    lib.bison_set_key.argtypes = [Handle, Hash, Hash]

    # ── Scalar setters (indexed) ────────────────────────────────────────────
    lib.bison_set_int_at.restype = Error
    lib.bison_set_int_at.argtypes = [Handle, ctypes.c_size_t, ctypes.c_int32]
    lib.bison_set_float_at.restype = Error
    lib.bison_set_float_at.argtypes = [Handle, ctypes.c_size_t, ctypes.c_float]
    lib.bison_set_string_at.restype = Error
    lib.bison_set_string_at.argtypes = [Handle, ctypes.c_size_t, ctypes.c_char_p]
    lib.bison_set_bool_at.restype = Error
    lib.bison_set_bool_at.argtypes = [Handle, ctypes.c_size_t, ctypes.c_int]
    lib.bison_set_key_at.restype = Error
    lib.bison_set_key_at.argtypes = [Handle, ctypes.c_size_t, Hash]
    lib.bison_set_object_at.restype = Error
    lib.bison_set_object_at.argtypes = [Handle, ctypes.c_size_t, Handle]

    # ── Scalar getters (named) ──────────────────────────────────────────────
    lib.bison_get_int.restype = Error
    lib.bison_get_int.argtypes = [Handle, Hash, P(ctypes.c_int32)]
    lib.bison_get_float.restype = Error
    lib.bison_get_float.argtypes = [Handle, Hash, P(ctypes.c_float)]
    lib.bison_get_bool.restype = Error
    lib.bison_get_bool.argtypes = [Handle, Hash, P(ctypes.c_int)]
    lib.bison_get_string.restype = Error
    lib.bison_get_string.argtypes = [Handle, Hash, ctypes.c_char_p, ctypes.c_size_t, P(ctypes.c_size_t)]
    lib.bison_get_object.restype = Error
    lib.bison_get_object.argtypes = [Handle, Hash, P(Handle)]
    lib.bison_get_key.restype = Error
    lib.bison_get_key.argtypes = [Handle, Hash, P(Hash)]

    # ── Scalar getters (indexed) ────────────────────────────────────────────
    lib.bison_get_int_at.restype = Error
    lib.bison_get_int_at.argtypes = [Handle, ctypes.c_size_t, P(ctypes.c_int32)]
    lib.bison_get_float_at.restype = Error
    lib.bison_get_float_at.argtypes = [Handle, ctypes.c_size_t, P(ctypes.c_float)]
    lib.bison_get_string_at.restype = Error
    lib.bison_get_string_at.argtypes = [Handle, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t, P(ctypes.c_size_t)]
    lib.bison_get_bool_at.restype = Error
    lib.bison_get_bool_at.argtypes = [Handle, ctypes.c_size_t, P(ctypes.c_int)]
    lib.bison_get_key_at.restype = Error
    lib.bison_get_key_at.argtypes = [Handle, ctypes.c_size_t, P(Hash)]
    lib.bison_get_object_at.restype = Error
    lib.bison_get_object_at.argtypes = [Handle, ctypes.c_size_t, P(Handle)]

    lib.bison_size.restype = ctypes.c_size_t
    lib.bison_size.argtypes = [Handle]

    # ── Methods ──────────────────────────────────────────────────────────────
    lib.bison_add_method.restype = Error
    lib.bison_add_method.argtypes = [Handle, Hash, MethodFn, ctypes.c_void_p, P(CAttributes)]

    lib.bison_call.restype = Error
    lib.bison_call.argtypes = [Handle, Hash, Handle, P(Handle)]

    # ── Field registration with metadata ────────────────────────────────────
    lib.bison_add_field_int.restype = Error
    lib.bison_add_field_int.argtypes = [Handle, Hash, ctypes.c_int32, P(CAttributes)]
    lib.bison_add_field_float.restype = Error
    lib.bison_add_field_float.argtypes = [Handle, Hash, ctypes.c_float, P(CAttributes)]
    lib.bison_add_field_bool.restype = Error
    lib.bison_add_field_bool.argtypes = [Handle, Hash, ctypes.c_int, P(CAttributes)]
    lib.bison_add_field_string.restype = Error
    lib.bison_add_field_string.argtypes = [Handle, Hash, ctypes.c_char_p, P(CAttributes)]
    lib.bison_add_field_key.restype = Error
    lib.bison_add_field_key.argtypes = [Handle, Hash, Hash, P(CAttributes)]

    lib.bison_add_field_vector_bool.restype = Error
    lib.bison_add_field_vector_bool.argtypes = [Handle, Hash, P(ctypes.c_int), ctypes.c_size_t, P(CAttributes)]
    lib.bison_add_field_vector_int.restype = Error
    lib.bison_add_field_vector_int.argtypes = [Handle, Hash, P(ctypes.c_int32), ctypes.c_size_t, P(CAttributes)]
    lib.bison_add_field_vector_float.restype = Error
    lib.bison_add_field_vector_float.argtypes = [Handle, Hash, P(ctypes.c_float), ctypes.c_size_t, P(CAttributes)]
    lib.bison_add_field_vector_bytes.restype = Error
    lib.bison_add_field_vector_bytes.argtypes = [Handle, Hash, P(ctypes.c_uint8), ctypes.c_size_t, P(CAttributes)]

    # ── Vector field access ─────────────────────────────────────────────────
    lib.bison_get_vector_bool.restype = Error
    lib.bison_get_vector_bool.argtypes = [Handle, Hash, P(ctypes.c_int), ctypes.c_size_t, P(ctypes.c_size_t)]
    lib.bison_get_vector_int.restype = Error
    lib.bison_get_vector_int.argtypes = [Handle, Hash, P(ctypes.c_int32), ctypes.c_size_t, P(ctypes.c_size_t)]
    lib.bison_get_vector_float.restype = Error
    lib.bison_get_vector_float.argtypes = [Handle, Hash, P(ctypes.c_float), ctypes.c_size_t, P(ctypes.c_size_t)]
    lib.bison_get_vector_bytes.restype = Error
    lib.bison_get_vector_bytes.argtypes = [Handle, Hash, P(ctypes.c_uint8), ctypes.c_size_t, P(ctypes.c_size_t)]

    lib.bison_set_vector_bool.restype = Error
    lib.bison_set_vector_bool.argtypes = [Handle, Hash, P(ctypes.c_int), ctypes.c_size_t]
    lib.bison_set_vector_int.restype = Error
    lib.bison_set_vector_int.argtypes = [Handle, Hash, P(ctypes.c_int32), ctypes.c_size_t]
    lib.bison_set_vector_float.restype = Error
    lib.bison_set_vector_float.argtypes = [Handle, Hash, P(ctypes.c_float), ctypes.c_size_t]
    lib.bison_set_vector_bytes.restype = Error
    lib.bison_set_vector_bytes.argtypes = [Handle, Hash, P(ctypes.c_uint8), ctypes.c_size_t]

    # ── Binary serialization ────────────────────────────────────────────────
    lib.bison_serialize.restype = Error
    lib.bison_serialize.argtypes = [Handle, P(P(ctypes.c_uint8)), P(ctypes.c_size_t)]
    lib.bison_deserialize.restype = Error
    lib.bison_deserialize.argtypes = [P(ctypes.c_uint8), ctypes.c_size_t, P(Handle)]
    lib.bison_free_buffer.restype = None
    lib.bison_free_buffer.argtypes = [P(ctypes.c_uint8)]

    # ── Utility ──────────────────────────────────────────────────────────────
    lib.bison_key.restype = Hash
    lib.bison_key.argtypes = [ctypes.c_char_p]

    # ── rmi_c.h: futures ─────────────────────────────────────────────────────
    lib.rmi_future_wait.restype = Error
    lib.rmi_future_wait.argtypes = [FutureHandle, ctypes.c_int64]

    lib.rmi_future_get_dynamic.restype = Error
    lib.rmi_future_get_dynamic.argtypes = [P(FutureHandle), P(Handle)]

    lib.rmi_future_get_proxy.restype = Error
    lib.rmi_future_get_proxy.argtypes = [P(FutureHandle), P(ProxyHandle)]

    lib.rmi_future_release.restype = None
    lib.rmi_future_release.argtypes = [FutureHandle]

    # ── rmi_c.h: client ──────────────────────────────────────────────────────
    lib.rmi_client_tcp_create.restype = ClientHandle
    lib.rmi_client_tcp_create.argtypes = [ctypes.c_char_p, ctypes.c_uint16]

    lib.rmi_client_pipe_create.restype = ClientHandle
    lib.rmi_client_pipe_create.argtypes = [ctypes.c_char_p]

    lib.rmi_client_term_create.restype = ClientHandle
    lib.rmi_client_term_create.argtypes = []

    lib.rmi_standalone_create.restype = ClientHandle
    lib.rmi_standalone_create.argtypes = []

    lib.rmi_client_connect.restype = Error
    lib.rmi_client_connect.argtypes = [ClientHandle, Handle]

    lib.rmi_client_describe.restype = Error
    lib.rmi_client_describe.argtypes = [ClientHandle, Hash, Hash, P(Handle)]

    lib.rmi_client_describe_async.restype = Error
    lib.rmi_client_describe_async.argtypes = [ClientHandle, Hash, Hash, P(FutureHandle)]

    lib.rmi_client_instantiate.restype = Error
    lib.rmi_client_instantiate.argtypes = [ClientHandle, Hash, Hash, Handle, P(ProxyHandle)]

    lib.rmi_client_instantiate_async.restype = Error
    lib.rmi_client_instantiate_async.argtypes = [ClientHandle, Hash, Hash, Handle, P(FutureHandle)]

    lib.rmi_client_disconnect.restype = Error
    lib.rmi_client_disconnect.argtypes = [ClientHandle]

    lib.rmi_client_release.restype = None
    lib.rmi_client_release.argtypes = [ClientHandle]

    # ── rmi_c.h: proxy ───────────────────────────────────────────────────────
    lib.rmi_proxy_release.restype = None
    lib.rmi_proxy_release.argtypes = [ProxyHandle]

    lib.rmi_proxy_on_event.restype = Error
    lib.rmi_proxy_on_event.argtypes = [ProxyHandle, Hash, ProxyEventFn, ctypes.c_void_p]

    lib.rmi_proxy_clear.restype = Error
    lib.rmi_proxy_clear.argtypes = [ProxyHandle, ctypes.c_int64]

    lib.rmi_proxy_clear_async.restype = Error
    lib.rmi_proxy_clear_async.argtypes = [ProxyHandle, P(FutureHandle)]

    lib.rmi_proxy_set.restype = Error
    lib.rmi_proxy_set.argtypes = [ProxyHandle, Handle, ctypes.c_int64]

    lib.rmi_proxy_set_async.restype = Error
    lib.rmi_proxy_set_async.argtypes = [ProxyHandle, Handle, P(FutureHandle)]

    lib.rmi_proxy_get.restype = Error
    lib.rmi_proxy_get.argtypes = [ProxyHandle, Handle, P(Handle), ctypes.c_int64]

    lib.rmi_proxy_get_async.restype = Error
    lib.rmi_proxy_get_async.argtypes = [ProxyHandle, Handle, P(FutureHandle)]

    lib.rmi_proxy_call.restype = Error
    lib.rmi_proxy_call.argtypes = [ProxyHandle, Hash, Handle, P(Handle), ctypes.c_int64]

    lib.rmi_proxy_call_async.restype = Error
    lib.rmi_proxy_call_async.argtypes = [ProxyHandle, Hash, Handle, P(FutureHandle)]

    # ── rmi_c.h: server ──────────────────────────────────────────────────────
    lib.rmi_server_tcp_create.restype = ServerHandle
    lib.rmi_server_tcp_create.argtypes = [ctypes.c_char_p, ctypes.c_uint16]

    lib.rmi_server_pipe_create.restype = ServerHandle
    lib.rmi_server_pipe_create.argtypes = [ctypes.c_char_p]

    lib.rmi_server_term_create.restype = ServerHandle
    lib.rmi_server_term_create.argtypes = [ctypes.c_char_p]

    lib.rmi_server_listen.restype = Error
    lib.rmi_server_listen.argtypes = [ServerHandle, Handle]

    lib.rmi_server_set_auth.restype = Error
    lib.rmi_server_set_auth.argtypes = [ServerHandle, AuthFn, ctypes.c_void_p]

    lib.rmi_server_stop.restype = None
    lib.rmi_server_stop.argtypes = [ServerHandle]

    lib.rmi_server_release.restype = None
    lib.rmi_server_release.argtypes = [ServerHandle]

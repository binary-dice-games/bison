// MIT License © 2025 Binary Dice Games
//
// bison_add_method / rmi_proxy_on_event / rmi_server_listen accept a raw
// `void* user` context pointer. Go's cgo pointer-passing rules disallow
// converting a runtime/cgo.Handle (an opaque uintptr) to unsafe.Pointer
// directly on the Go side (go vet's unsafeptr check flags it), so these
// shims accept `uintptr_t` from Go and perform the uintptr_t->void* cast
// entirely on the C side instead.
//
// These live in an ordinary .c file (compiled alongside the Go sources by
// cgo automatically) rather than as function bodies inside a Go file's cgo
// preamble comment: cgo re-emits a preamble comment's *entire* text
// (definitions included, not just declarations) into the generated
// _cgo_export.c for any file that also contains `//export` trampolines,
// which would otherwise duplicate-define these functions and fail to link.
#include "shim.h"

bison_error bison_add_method_shim(
    bison_handle h,
    bison_hash name,
    bison_method_fn fn,
    uintptr_t user,
    const bison_attributes* meta) {
  return bison_add_method(h, name, fn, (void*)user, meta);
}

rmi_error rmi_proxy_on_event_shim(
    rmi_proxy_handle proxy,
    bison_hash event_name,
    rmi_proxy_event_fn handler,
    uintptr_t user) {
  return rmi_proxy_on_event(proxy, event_name, handler, (void*)user);
}

rmi_error rmi_server_listen_shim(
    rmi_server_handle server,
    bison_handle params,
    rmi_auth_fn auth_handler,
    uintptr_t auth_user) {
  return rmi_server_listen(server, params, auth_handler, (void*)auth_user);
}

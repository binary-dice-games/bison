// MIT License © 2025 Binary Dice Games
// Small C shims bridging bison_c.h/rmi_c.h's `void* user` callback context
// parameters to Go's runtime/cgo.Handle (an opaque uintptr) -- see shim.c
// for why these exist as real C functions rather than as bodies inside a Go
// cgo preamble comment.
#ifndef BISON_GO_SHIM_H
#define BISON_GO_SHIM_H

#include <stdint.h>

#include "bison_c.h"
#include "rmi_c.h"

bison_error bison_add_method_shim(
    bison_handle h,
    bison_hash name,
    bison_method_fn fn,
    uintptr_t user,
    const bison_attributes* meta);

rmi_error rmi_proxy_on_event_shim(
    rmi_proxy_handle proxy,
    bison_hash event_name,
    rmi_proxy_event_fn handler,
    uintptr_t user);

rmi_error rmi_server_listen_shim(
    rmi_server_handle server,
    bison_handle params,
    rmi_auth_fn auth_handler,
    uintptr_t auth_user);

#endif /* BISON_GO_SHIM_H */

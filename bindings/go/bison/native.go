// MIT License © 2025 Binary Dice Games

// Package bison provides Go bindings for Bison
// (https://github.com/binary-dice-games/bison), a C++20 library for
// serializing dynamic, self-describing objects to a compact binary format,
// plus RMI (Remote Method Invocation) over TCP, TLS, named-pipe, and stdio
// transports.
//
// This package links directly against the precompiled bison_abi shared
// library at build time via cgo (see this file's preamble below) -- the
// same model bindings/cpp/ and bindings/rust/ use, unlike the Python and C#
// bindings, which dlopen/P-Invoke it at run time. cgo is also the only
// practical way for this package to hand C a function pointer that calls
// back into Go (bison_method_fn, rmi_proxy_event_fn, rmi_auth_fn all need
// this), since Go's //export mechanism requires it.
//
// [Dynamic] (in dynamic.go) is the safe wrapper around bison_c.h; [Client],
// [Server], [Proxy], and [Future] (in rmi.go) wrap rmi_c.h. This file
// (native.go) carries the cgo preamble and the C-callable trampolines that
// dispatch method/event/auth callbacks into Go closures; native.go, dynamic.go,
// and rmi.go each carry their own `import "C"` (cgo preambles are per file,
// but the #cgo CFLAGS/LDFLAGS directives declared here apply package-wide,
// so the other files only need to repeat the header #includes).
//
// # Quick start
//
//	p, _ := bison.New("Player")
//	defer p.Close()
//	p.SetInt("hp", 100)
//	p.SetString("name", "hero")
//	hp, _ := p.GetInt("hp") // 100
//
//	client := bison.NewStandaloneClient()
//	defer client.Close()
//	client.Connect(nil)
//	calc, _ := client.Instantiate("Calculator", "", nil)
//	defer calc.Close()
//	args, _ := bison.New("")
//	defer args.Close()
//	args.SetFloat("a", 1.0)
//	args.SetFloat("b", 2.0)
//	result, _ := calc.Call("add", args, -1) // calls the "add" remote method
//	defer result.Close()
//	sum, _ := result.GetFloat("result")
//
// # Overriding the library location
//
// The default #cgo directives below resolve bison_c.h/rmi_c.h and
// libbison_abi against this checkout's sibling include/ and build/
// directories (three levels up from bindings/go/bison/). To build against a
// bison_abi installed elsewhere, set the CGO_CFLAGS/CGO_LDFLAGS environment
// variables before `go build`/`go test` -- the Go toolchain automatically
// merges env-supplied CGO_CFLAGS/CGO_LDFLAGS with the #cgo directives below.
// This is the Go-native equivalent of every other binding's BISON_LIB
// override:
//
//	export CGO_CFLAGS="-I/path/to/bison/include"
//	export CGO_LDFLAGS="-L/path/to/bison/build -lbison_abi -Wl,-rpath,/path/to/bison/build"
//	go build ./...
package bison

/*
#cgo CFLAGS: -I${SRCDIR}/../../../include
#cgo LDFLAGS: -L${SRCDIR}/../../../build -lbison_abi -Wl,-rpath,${SRCDIR}/../../../build
#include <stdlib.h>
#include "bison_c.h"
#include "rmi_c.h"
#include "shim.h"

// Forward declarations so shim.c's registration calls (and this package's
// other cgo files) can reference these //export'd Go functions as C
// function pointers before cgo's generated header is available.
extern void goMethodTrampoline(bison_handle self, bison_handle params, bison_handle result, void* user);
extern void goProxyEventTrampoline(bison_handle params, void* user);
extern bool goAuthTrampoline(bison_handle payload, char* identity_buf, size_t identity_buf_len, void* user);
*/
import "C"

import (
	"runtime/cgo"
	"unsafe"
)

// goMethodTrampoline is the single C-callable entry point for every method
// registered via Dynamic.AddMethod in this process; `user` is a
// runtime/cgo.Handle (see the shim above) that resolves to the registered
// Go closure. Panics are recovered here and swallowed -- they must never
// cross back into C++ (this mirrors the "exceptions must not cross the C
// ABI boundary" rule documented in bison_c.h).
//
//export goMethodTrampoline
func goMethodTrampoline(self, params, result C.bison_handle, user unsafe.Pointer) {
	defer func() { _ = recover() }()
	h := cgo.Handle(uintptr(user))
	fn, ok := h.Value().(func(self, params, result *Dynamic))
	if !ok {
		return
	}
	selfDyn := &Dynamic{handle: self, owned: false}
	paramsDyn := &Dynamic{handle: params, owned: false}
	resultDyn := &Dynamic{handle: result, owned: false}
	fn(selfDyn, paramsDyn, resultDyn)
}

// goProxyEventTrampoline is the C-callable entry point for every handler
// registered via Proxy.OnEvent. See goMethodTrampoline's doc comment for the
// user-handle and panic-recovery conventions, which are identical here.
//
//export goProxyEventTrampoline
func goProxyEventTrampoline(params C.bison_handle, user unsafe.Pointer) {
	defer func() { _ = recover() }()
	h := cgo.Handle(uintptr(user))
	fn, ok := h.Value().(func(params *Dynamic))
	if !ok {
		return
	}
	paramsDyn := &Dynamic{handle: params, owned: false}
	fn(paramsDyn)
}

// goAuthTrampoline is the C-callable entry point for the auth callback
// registered via Server.Listen. A panic in the Go handler is treated as a
// rejection (matching Python's `except Exception: return False`), rather
// than allowing it to unwind into C++.
//
//export goAuthTrampoline
func goAuthTrampoline(payload C.bison_handle, identityBuf *C.char, identityBufLen C.size_t, user unsafe.Pointer) C.bool {
	h := cgo.Handle(uintptr(user))
	fn, ok := h.Value().(func(payload *Dynamic) (bool, string))
	if !ok {
		return C.bool(false)
	}

	var accepted bool
	var identity string
	func() {
		defer func() { _ = recover() }()
		payloadDyn := &Dynamic{handle: payload, owned: false}
		accepted, identity = fn(payloadDyn)
	}()

	if accepted && identity != "" && identityBufLen > 0 {
		encoded := []byte(identity)
		n := len(encoded)
		if maxN := int(identityBufLen) - 1; n > maxN {
			n = maxN
		}
		if n > 0 {
			dst := unsafe.Slice((*byte)(unsafe.Pointer(identityBuf)), int(identityBufLen))
			copy(dst, encoded[:n])
			dst[n] = 0
		} else {
			*identityBuf = 0
		}
	}
	return C.bool(accepted)
}

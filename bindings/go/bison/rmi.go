// MIT License © 2025 Binary Dice Games

// Package bison: this file is the safe, idiomatic Go wrapper around
// rmi_c.h -- the Bison RMI C ABI. It provides Client/Server (wrappers over
// rmi_client_handle/rmi_server_handle) plus Proxy, a live handle to a
// remote (or in-process standalone) object, and Future for the *Async
// operations. Every wrapper releases its underlying handle on Close (also
// backstopped by a runtime.SetFinalizer, per dynamic.go's caveat about
// finalizer timing).
package bison

/*
#include <stdlib.h>
#include "rmi_c.h"
#include "shim.h"

// Forward declarations for the //export'd trampolines defined in native.go
// (see shim.c/shim.h for rmi_proxy_on_event_shim/rmi_server_listen_shim
// themselves).
extern void goProxyEventTrampoline(bison_handle params, void* user);
extern bool goAuthTrampoline(bison_handle payload, char* identity_buf, size_t identity_buf_len, void* user);
*/
import "C"

import (
	"fmt"
	"runtime"
	"runtime/cgo"
	"sync"
	"unsafe"
)

func paramsHandle(params *Dynamic) C.bison_handle {
	if params == nil {
		return nil
	}
	return params.handle
}

// ─── Errors ─────────────────────────────────────────────────────────────────

// rmi_error codes (see rmi_c.h), exported as typed constants so callers can
// compare an *RmiError's Code field directly.
const (
	RmiErrNull            int32 = -1 // A required handle or pointer argument was NULL.
	RmiErrInvalidState    int32 = -2 // Operation invalid for current state (e.g. not connected).
	RmiErrTimeout         int32 = -3 // Request timed out.
	RmiErrRemoteException int32 = -4 // Server raised an exception.
	RmiErrTransport       int32 = -5 // Transport error (network, connection, etc.).
	RmiErrException       int32 = -6 // An unexpected C++ exception was caught.
)

var rmiErrorMessages = map[int32]string{
	RmiErrNull:            "Null handle or pointer",
	RmiErrInvalidState:    "Operation invalid for current state",
	RmiErrTimeout:         "Request timed out",
	RmiErrRemoteException: "Server raised an exception",
	RmiErrTransport:       "Transport error",
	RmiErrException:       "Internal C++ exception",
}

func rmiErrorMessage(code int32) string {
	if msg, ok := rmiErrorMessages[code]; ok {
		return msg
	}
	return fmt.Sprintf("Unknown error %d", code)
}

// RmiError is returned when an rmi_* C API call reports a non-zero error
// code. Code is the raw rmi_error value (see rmi_c.h); compare it against
// the RmiErr* constants, or use errors.As to recover an *RmiError from a
// wrapped error.
type RmiError struct {
	Code    int32
	message string
}

func (e *RmiError) Error() string { return e.message }

func newRmiError(code int32, context string) *RmiError {
	msg := rmiErrorMessage(code)
	if context != "" {
		msg = context + ": " + msg
	}
	return &RmiError{Code: code, message: msg}
}

func checkRmi(rc C.rmi_error, context string) error {
	if rc == C.RMI_OK {
		return nil
	}
	return newRmiError(int32(rc), context)
}

// ─── Future ─────────────────────────────────────────────────────────────────

// Future wraps an rmi_future_handle for an in-flight asynchronous
// operation. It is consumed exactly once, via GetDynamic or GetProxy (both
// nil the internal handle immediately, so a second call to either returns
// an error rather than double-consuming the future) -- or discarded via
// Release, also safe to call multiple times.
type Future struct {
	handle C.rmi_future_handle
}

func newFuture(h C.rmi_future_handle) *Future {
	f := &Future{handle: h}
	runtime.SetFinalizer(f, (*Future).finalize)
	return f
}

func (f *Future) finalize() { f.Release() }

// Wait blocks until the operation completes. It does not consume the
// future.
func (f *Future) Wait(timeoutMs int64) error {
	rc := C.rmi_future_wait(f.handle, C.int64_t(timeoutMs))
	return checkRmi(rc, "future.wait")
}

// GetDynamic consumes the future and returns its Dynamic result.
func (f *Future) GetDynamic() (*Dynamic, error) {
	if f.handle == nil {
		return nil, fmt.Errorf("bison: future already consumed")
	}
	var out C.bison_handle
	rc := C.rmi_future_get_dynamic(&f.handle, &out)
	f.handle = nil
	if err := checkRmi(rc, "future.get_dynamic"); err != nil {
		return nil, err
	}
	return newOwned(out), nil
}

// GetProxy consumes the future and returns its Proxy result.
func (f *Future) GetProxy() (*Proxy, error) {
	if f.handle == nil {
		return nil, fmt.Errorf("bison: future already consumed")
	}
	var out C.rmi_proxy_handle
	rc := C.rmi_future_get_proxy(&f.handle, &out)
	f.handle = nil
	if err := checkRmi(rc, "future.get_proxy"); err != nil {
		return nil, err
	}
	return newProxy(out), nil
}

// Release discards the future without consuming its result. Safe to call
// multiple times.
func (f *Future) Release() {
	if f.handle != nil {
		C.rmi_future_release(f.handle)
		f.handle = nil
	}
}

// ─── Proxy ──────────────────────────────────────────────────────────────────

// Proxy is a live handle to a remote (or in-process standalone) object.
// Get/Set project/patch fields; Call invokes a remote method by name.
// Close sends a destroy request to the server (rmi_proxy_release).
type Proxy struct {
	handle    C.rmi_proxy_handle
	callbacks []cgo.Handle // OnEvent registrations, freed on Close.
}

func newProxy(h C.rmi_proxy_handle) *Proxy {
	p := &Proxy{handle: h}
	runtime.SetFinalizer(p, (*Proxy).finalize)
	return p
}

func (p *Proxy) finalize() { _ = p.Close() }

// Close destroys the remote object and releases this proxy. Safe to call
// multiple times.
func (p *Proxy) Close() error {
	if p.handle != nil {
		C.rmi_proxy_release(p.handle)
		p.handle = nil
	}
	for _, h := range p.callbacks {
		h.Delete()
	}
	p.callbacks = nil
	return nil
}

// Get fetches a full snapshot, or a projected subset of fields when
// projection is non-nil.
func (p *Proxy) Get(projection *Dynamic, timeoutMs int64) (*Dynamic, error) {
	var out C.bison_handle
	rc := C.rmi_proxy_get(p.handle, paramsHandle(projection), &out, C.int64_t(timeoutMs))
	if err := checkRmi(rc, "proxy.get"); err != nil {
		return nil, err
	}
	return newOwned(out), nil
}

// GetAsync is the asynchronous counterpart to Get.
func (p *Proxy) GetAsync(projection *Dynamic) (*Future, error) {
	var out C.rmi_future_handle
	rc := C.rmi_proxy_get_async(p.handle, paramsHandle(projection), &out)
	if err := checkRmi(rc, "proxy.get_async"); err != nil {
		return nil, err
	}
	return newFuture(out), nil
}

// Set applies a partial field update without resetting unspecified fields.
func (p *Proxy) Set(fields *Dynamic, timeoutMs int64) error {
	rc := C.rmi_proxy_set(p.handle, paramsHandle(fields), C.int64_t(timeoutMs))
	return checkRmi(rc, "proxy.set")
}

// SetAsync is the asynchronous counterpart to Set.
func (p *Proxy) SetAsync(fields *Dynamic) (*Future, error) {
	var out C.rmi_future_handle
	rc := C.rmi_proxy_set_async(p.handle, paramsHandle(fields), &out)
	if err := checkRmi(rc, "proxy.set_async"); err != nil {
		return nil, err
	}
	return newFuture(out), nil
}

// Clear resets explicitly-set fields back to prototype/inherited defaults.
func (p *Proxy) Clear(timeoutMs int64) error {
	rc := C.rmi_proxy_clear(p.handle, C.int64_t(timeoutMs))
	return checkRmi(rc, "proxy.clear")
}

// ClearAsync is the asynchronous counterpart to Clear.
func (p *Proxy) ClearAsync() (*Future, error) {
	var out C.rmi_future_handle
	rc := C.rmi_proxy_clear_async(p.handle, &out)
	if err := checkRmi(rc, "proxy.clear_async"); err != nil {
		return nil, err
	}
	return newFuture(out), nil
}

// Call invokes a named remote method with params (nil for no arguments).
func (p *Proxy) Call(name string, params *Dynamic, timeoutMs int64) (*Dynamic, error) {
	var out C.bison_handle
	rc := C.rmi_proxy_call(p.handle, cKey(name), paramsHandle(params), &out, C.int64_t(timeoutMs))
	if err := checkRmi(rc, fmt.Sprintf("proxy.call(%q)", name)); err != nil {
		return nil, err
	}
	return newOwned(out), nil
}

// CallAsync is the asynchronous counterpart to Call.
func (p *Proxy) CallAsync(name string, params *Dynamic) (*Future, error) {
	var out C.rmi_future_handle
	rc := C.rmi_proxy_call_async(p.handle, cKey(name), paramsHandle(params), &out)
	if err := checkRmi(rc, fmt.Sprintf("proxy.call_async(%q)", name)); err != nil {
		return nil, err
	}
	return newFuture(out), nil
}

// OnEvent subscribes to a server-initiated event on this object. handler
// receives a non-owning view of the event payload and may be invoked from
// any goroutine/thread that receives the push event, not necessarily the
// one that called OnEvent; a panic inside handler is recovered and
// swallowed at the C ABI boundary.
func (p *Proxy) OnEvent(name string, handler func(params *Dynamic)) error {
	h := cgo.NewHandle(handler)
	rc := C.rmi_proxy_on_event_shim(p.handle, cKey(name), C.rmi_proxy_event_fn(C.goProxyEventTrampoline), C.uintptr_t(h))
	if rc != C.RMI_OK {
		h.Delete()
		return checkRmi(rc, fmt.Sprintf("on_event(%q)", name))
	}
	p.callbacks = append(p.callbacks, h)
	return nil
}

// ─── Client ─────────────────────────────────────────────────────────────────

// Client wraps an rmi_client_handle. Construct via NewTCPClient,
// NewTLSClient, NewPipeClient, NewTermClient, or NewStandaloneClient.
type Client struct {
	handle    C.rmi_client_handle
	connected bool
}

func newClient(h C.rmi_client_handle) *Client {
	c := &Client{handle: h}
	runtime.SetFinalizer(c, (*Client).finalize)
	return c
}

func (c *Client) finalize() { _ = c.Close() }

// NewTCPClient creates a TCP socket client. The client is not connected
// until Connect is called.
func NewTCPClient(host string, port uint16) (*Client, error) {
	cHost := C.CString(host)
	defer C.free(unsafe.Pointer(cHost))
	h := C.rmi_client_tcp_create(cHost, C.uint16_t(port))
	if h == nil {
		return nil, fmt.Errorf("bison: rmi_client_tcp_create failed")
	}
	return newClient(h), nil
}

// NewTLSClient creates a TLS-secured TCP socket client (not yet connected).
// TLS trust/identity material (ca_file/ca_pem, insecure_skip_verify,
// cert_file/cert_pem, key_file/key_pem, key_password, server_name) is
// supplied via Connect's params.
func NewTLSClient(host string, port uint16) (*Client, error) {
	cHost := C.CString(host)
	defer C.free(unsafe.Pointer(cHost))
	h := C.rmi_client_tls_create(cHost, C.uint16_t(port))
	if h == nil {
		return nil, fmt.Errorf("bison: rmi_client_tls_create failed")
	}
	return newClient(h), nil
}

// NewPipeClient creates a named-pipe / Unix-socket client. On Windows, path
// is a full pipe path (\\.\pipe\name); on Linux/macOS it is a file-system
// socket path.
func NewPipeClient(path string) (*Client, error) {
	cPath := C.CString(path)
	defer C.free(unsafe.Pointer(cPath))
	h := C.rmi_client_pipe_create(cPath)
	if h == nil {
		return nil, fmt.Errorf("bison: rmi_client_pipe_create failed")
	}
	return newClient(h), nil
}

// NewTermClient creates a terminal (OSC-99 framed) client, wrapping the
// calling process's own inherited stdio. Intended for a client process
// spawned by NewTermServer's server.
func NewTermClient() (*Client, error) {
	h := C.rmi_client_term_create()
	if h == nil {
		return nil, fmt.Errorf("bison: rmi_client_term_create failed")
	}
	return newClient(h), nil
}

// NewStandaloneClient creates an in-process client dispatching directly to
// the local class registry. Connect/Disconnect are accepted but are no-ops.
func NewStandaloneClient() (*Client, error) {
	h := C.rmi_standalone_create()
	if h == nil {
		return nil, fmt.Errorf("bison: rmi_standalone_create failed")
	}
	return newClient(h), nil
}

// Connect opens the transport and starts the background worker for
// receiving responses.
func (c *Client) Connect(params *Dynamic) error {
	rc := C.rmi_client_connect(c.handle, paramsHandle(params))
	if err := checkRmi(rc, "connect"); err != nil {
		return err
	}
	c.connected = true
	return nil
}

// Disconnect closes the transport and stops worker threads. A no-op if not
// currently connected.
func (c *Client) Disconnect() error {
	if !c.connected {
		return nil
	}
	rc := C.rmi_client_disconnect(c.handle)
	if err := checkRmi(rc, "disconnect"); err != nil {
		return err
	}
	c.connected = false
	return nil
}

// Close disconnects (if connected) and releases the client handle. Safe to
// call multiple times.
func (c *Client) Close() error {
	if c.connected {
		C.rmi_client_disconnect(c.handle)
		c.connected = false
	}
	if c.handle != nil {
		C.rmi_client_release(c.handle)
		c.handle = nil
	}
	return nil
}

// Instantiate creates a remote object on the server and returns a Proxy to
// it.
func (c *Client) Instantiate(klassName, nsName string, params *Dynamic) (*Proxy, error) {
	var out C.rmi_proxy_handle
	rc := C.rmi_client_instantiate(c.handle, cKeyOrZero(nsName), cKey(klassName), paramsHandle(params), &out)
	if err := checkRmi(rc, fmt.Sprintf("instantiate(%q)", klassName)); err != nil {
		return nil, err
	}
	return newProxy(out), nil
}

// InstantiateAsync is the asynchronous counterpart to Instantiate.
func (c *Client) InstantiateAsync(klassName, nsName string, params *Dynamic) (*Future, error) {
	var out C.rmi_future_handle
	rc := C.rmi_client_instantiate_async(c.handle, cKeyOrZero(nsName), cKey(klassName), paramsHandle(params), &out)
	if err := checkRmi(rc, fmt.Sprintf("instantiate_async(%q)", klassName)); err != nil {
		return nil, err
	}
	return newFuture(out), nil
}

// Describe requests class metadata from the server. An empty klassName
// queries all metadata.
func (c *Client) Describe(klassName, nsName string) (*Dynamic, error) {
	var out C.bison_handle
	rc := C.rmi_client_describe(c.handle, cKeyOrZero(nsName), cKeyOrZero(klassName), &out)
	if err := checkRmi(rc, "describe"); err != nil {
		return nil, err
	}
	return newOwned(out), nil
}

// DescribeAsync is the asynchronous counterpart to Describe.
func (c *Client) DescribeAsync(klassName, nsName string) (*Future, error) {
	var out C.rmi_future_handle
	rc := C.rmi_client_describe_async(c.handle, cKeyOrZero(nsName), cKeyOrZero(klassName), &out)
	if err := checkRmi(rc, "describe_async"); err != nil {
		return nil, err
	}
	return newFuture(out), nil
}

// ─── Server ─────────────────────────────────────────────────────────────────

// Server wraps an rmi_server_handle. Construct via NewTCPServer,
// NewTLSServer, NewPipeServer, or NewTermServer.
type Server struct {
	handle    C.rmi_server_handle
	listening bool
	callbacks []cgo.Handle // The Listen auth callback, if any, freed on Close.
}

func newServer(h C.rmi_server_handle) *Server {
	s := &Server{handle: h}
	runtime.SetFinalizer(s, (*Server).finalize)
	return s
}

func (s *Server) finalize() { _ = s.Close() }

// NewTCPServer creates a TCP socket server listener. The server is not
// listening until Listen is called.
func NewTCPServer(host string, port uint16) (*Server, error) {
	cHost := C.CString(host)
	defer C.free(unsafe.Pointer(cHost))
	h := C.rmi_server_tcp_create(cHost, C.uint16_t(port))
	if h == nil {
		return nil, fmt.Errorf("bison: rmi_server_tcp_create failed")
	}
	return newServer(h), nil
}

// NewTLSServer creates a TLS-secured TCP socket server listener (not yet
// listening). Server certificate/key material (cert_file/cert_pem,
// key_file/key_pem, key_password) and optional mutual-TLS settings
// (client_auth, ca_file/ca_pem) are supplied via Listen's params.
func NewTLSServer(host string, port uint16) (*Server, error) {
	cHost := C.CString(host)
	defer C.free(unsafe.Pointer(cHost))
	h := C.rmi_server_tls_create(cHost, C.uint16_t(port))
	if h == nil {
		return nil, fmt.Errorf("bison: rmi_server_tls_create failed")
	}
	return newServer(h), nil
}

// NewPipeServer creates a named-pipe / Unix-socket server listener.
func NewPipeServer(path string) (*Server, error) {
	cPath := C.CString(path)
	defer C.free(unsafe.Pointer(cPath))
	h := C.rmi_server_pipe_create(cPath)
	if h == nil {
		return nil, fmt.Errorf("bison: rmi_server_pipe_create failed")
	}
	return newServer(h), nil
}

// NewTermServer creates a terminal (OSC-99 framed) server listener,
// spawning a child process attached to a new pseudo-terminal. cmd is the
// command to exec in the spawned child; pass "" to spawn the operator's
// default shell.
func NewTermServer(cmd string) (*Server, error) {
	var cCmd *C.char
	if cmd != "" {
		cCmd = C.CString(cmd)
		defer C.free(unsafe.Pointer(cCmd))
	}
	h := C.rmi_server_term_create(cCmd)
	if h == nil {
		return nil, fmt.Errorf("bison: rmi_server_term_create failed")
	}
	return newServer(h), nil
}

// Listen starts accepting client connections and spawns background worker
// threads. auth, if non-nil, is evaluated once per incoming connection for
// as long as the server keeps listening -- it can only be set here, not
// changed afterward. It receives the client's OP_CONNECT payload (a
// non-owning view) and returns (accepted, identity): whether to accept the
// connection, and (if accepted) an identity string. Identity strings longer
// than 255 UTF-8 bytes are truncated (the C ABI uses a fixed 256-byte
// buffer). A panic inside auth is treated as a rejection.
func (s *Server) Listen(params *Dynamic, auth func(payload *Dynamic) (bool, string)) error {
	var fn C.rmi_auth_fn
	var userHandle C.uintptr_t
	if auth != nil {
		h := cgo.NewHandle(auth)
		s.callbacks = append(s.callbacks, h)
		fn = C.rmi_auth_fn(C.goAuthTrampoline)
		userHandle = C.uintptr_t(h)
	}
	rc := C.rmi_server_listen_shim(s.handle, paramsHandle(params), fn, userHandle)
	if err := checkRmi(rc, "listen"); err != nil {
		return err
	}
	s.listening = true
	return nil
}

// Stop closes the listener socket and stops accepting new connections. A
// no-op if not currently listening.
func (s *Server) Stop() {
	if s.listening {
		C.rmi_server_stop(s.handle)
		s.listening = false
	}
}

// Close stops the server (if listening) and releases the server handle.
// Safe to call multiple times.
func (s *Server) Close() error {
	s.Stop()
	if s.handle != nil {
		C.rmi_server_release(s.handle)
		s.handle = nil
	}
	for _, h := range s.callbacks {
		h.Delete()
	}
	s.callbacks = nil
	return nil
}

// ── Profiling ────────────────────────────────────────────────────────────

// EnableProfiling opts this server into Perfetto trace capture. Must be
// called before StartCapture.
func (s *Server) EnableProfiling(outputDir string) error {
	cDir := C.CString(outputDir)
	defer C.free(unsafe.Pointer(cDir))
	rc := C.rmi_server_enable_profiling(s.handle, cDir)
	return checkRmi(rc, "enable_profiling")
}

// StartCapture starts Perfetto capture. Returns whether capture is active
// afterward (false if EnableProfiling was never called). label is an
// optional human-readable label embedded in the trace file name.
func (s *Server) StartCapture(label string) (bool, error) {
	var cLabel *C.char
	if label != "" {
		cLabel = C.CString(label)
		defer C.free(unsafe.Pointer(cLabel))
	}
	var started C.bool
	rc := C.rmi_server_start_capture(s.handle, cLabel, &started)
	if err := checkRmi(rc, "start_capture"); err != nil {
		return false, err
	}
	return bool(started), nil
}

// StopCapture stops Perfetto capture and finalizes the trace file. A no-op
// if capture is not currently active.
func (s *Server) StopCapture() error {
	rc := C.rmi_server_stop_capture(s.handle)
	return checkRmi(rc, "stop_capture")
}

// IsCaptureActive reports whether this server currently has Perfetto
// capture active.
func (s *Server) IsCaptureActive() (bool, error) {
	var active C.bool
	rc := C.rmi_server_is_capture_active(s.handle, &active)
	if err := checkRmi(rc, "is_capture_active"); err != nil {
		return false, err
	}
	return bool(active), nil
}

// ─── Profiling: free-function trace calls ──────────────────────────────────

// traceNames interns each unique trace name's C string forever. The
// rmi_trace_* functions take a `const char*` the library does not copy on
// the hot path -- the header requires the pointer to outlive the traced
// slice/event -- so freeing it after a single call would leave a dangling
// pointer once a later call (e.g. TraceScopeEnd, well after TraceScopeBegin
// returns) reads it back. Never evicted, matching Rust's identical cache.
var (
	traceNamesMu sync.Mutex
	traceNames   = make(map[string]*C.char)
)

func traceNamePtr(name string) *C.char {
	traceNamesMu.Lock()
	defer traceNamesMu.Unlock()
	if p, ok := traceNames[name]; ok {
		return p
	}
	p := C.CString(name)
	traceNames[name] = p
	return p
}

// TraceScopeBegin begins a named trace slice on the calling goroutine's
// thread. Every call must be matched by a later TraceScopeEnd. No-op if no
// server or client in this process currently has an active profiling
// recorder.
func TraceScopeBegin(name string) {
	C.rmi_trace_scope_begin(traceNamePtr(name))
}

// TraceScopeEnd ends the most recently begun trace slice.
func TraceScopeEnd() {
	C.rmi_trace_scope_end()
}

// TraceInstant records a zero-duration named instant event.
func TraceInstant(name string) {
	C.rmi_trace_instant(traceNamePtr(name))
}

// TraceCounterInt records an integer-valued sample on a named counter
// track. All samples sharing name land on the same track regardless of
// which thread records them.
func TraceCounterInt(name string, value int64) {
	C.rmi_trace_counter_int(traceNamePtr(name), C.int64_t(value))
}

// TraceCounterDouble records a floating-point sample on a named counter
// track.
func TraceCounterDouble(name string, value float64) {
	C.rmi_trace_counter_double(traceNamePtr(name), C.double(value))
}

// TraceIsActive reports whether this process currently has an active
// profiling recorder (server or client) that trace calls will actually be
// recorded to.
func TraceIsActive() bool {
	return bool(C.rmi_trace_is_active())
}

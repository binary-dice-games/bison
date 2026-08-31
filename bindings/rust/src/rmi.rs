// MIT License © 2025 Binary Dice Games
//! Safe, idiomatic wrapper around `rmi_c.h` -- the Bison RMI C ABI.
//!
//! Provides [`Client`] / [`Server`] (RAII wrappers over `rmi_client_handle`
//! / `rmi_server_handle`) plus [`Proxy`], a live handle to a remote (or
//! in-process standalone) object, and [`Future`] for the `_async`
//! operations. Every wrapper releases its underlying handle on [`Drop`].

use std::collections::HashMap;
use std::ffi::CString;
use std::fmt;
use std::os::raw::{c_char, c_void};
use std::ptr;
use std::sync::{Mutex, OnceLock};

use crate::dynamic::{key, key_or_zero, Dynamic};
use crate::sys;

// ─── Errors ─────────────────────────────────────────────────────────────────

/// Raised when an `rmi_*` C API call returns a non-zero error code.
#[derive(Debug, Clone)]
pub struct RmiError {
    /// The raw `rmi_error` code (see `rmi_c.h`).
    pub code: i32,
    message: String,
}

impl RmiError {
    fn new(code: i32, context: &str) -> Self {
        let msg = error_message(code);
        let message = if context.is_empty() {
            msg.to_string()
        } else {
            format!("{context}: {msg}")
        };
        RmiError { code, message }
    }
}

impl fmt::Display for RmiError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.message)
    }
}

impl std::error::Error for RmiError {}

fn error_message(code: i32) -> &'static str {
    match code {
        sys::RMI_ERR_NULL => "Null handle or pointer",
        sys::RMI_ERR_INVALID_STATE => "Operation invalid for current state",
        sys::RMI_ERR_TIMEOUT => "Request timed out",
        sys::RMI_ERR_REMOTE_EXCEPTION => "Server raised an exception",
        sys::RMI_ERR_TRANSPORT => "Transport error",
        sys::RMI_ERR_EXCEPTION => "Internal C++ exception",
        _ => "Unknown error",
    }
}

fn check(rc: sys::rmi_error, context: &str) -> Result<(), RmiError> {
    if rc == sys::RMI_OK {
        Ok(())
    } else {
        Err(RmiError::new(rc, context))
    }
}

fn params_handle(params: Option<&Dynamic>) -> sys::bison_handle {
    params.map_or(ptr::null_mut(), Dynamic::raw_handle)
}

// ─── Future ─────────────────────────────────────────────────────────────────

/// RAII wrapper around an `rmi_future_handle`. Consumed exactly once via
/// [`Future::get_dynamic`] or [`Future::get_proxy`] (both take `self` by
/// value, so the C ABI's "set to null on consume" rule is enforced by Rust's
/// ownership system instead of a runtime check), or discarded by dropping.
pub struct Future {
    handle: sys::rmi_future_handle,
}

unsafe impl Send for Future {}

impl Future {
    pub(crate) fn from_raw(handle: sys::rmi_future_handle) -> Self {
        Future { handle }
    }

    /// Blocks until the operation completes. Does not consume the future.
    pub fn wait(&self, timeout_ms: i64) -> Result<(), RmiError> {
        let rc = unsafe { sys::rmi_future_wait(self.handle, timeout_ms) };
        check(rc, "future.wait")
    }

    /// Consumes the future and returns its [`Dynamic`] result.
    pub fn get_dynamic(mut self) -> Result<Dynamic, RmiError> {
        let mut out: sys::bison_handle = ptr::null_mut();
        let rc = unsafe { sys::rmi_future_get_dynamic(&mut self.handle, &mut out) };
        check(rc, "future.get_dynamic")?;
        Ok(Dynamic::from_raw(out, true))
    }

    /// Consumes the future and returns its [`Proxy`] result.
    pub fn get_proxy(mut self) -> Result<Proxy, RmiError> {
        let mut out: sys::rmi_proxy_handle = ptr::null_mut();
        let rc = unsafe { sys::rmi_future_get_proxy(&mut self.handle, &mut out) };
        check(rc, "future.get_proxy")?;
        Ok(Proxy::from_raw(out))
    }
}

impl Drop for Future {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            unsafe { sys::rmi_future_release(self.handle) };
            self.handle = ptr::null_mut();
        }
    }
}

// ─── Proxy ──────────────────────────────────────────────────────────────────

type EventCallback = dyn FnMut(&Dynamic) + Send;

/// A live handle to a remote (or in-process standalone) object.
///
/// [`Proxy::get`]/[`Proxy::set`] project/patch fields; [`Proxy::call`]
/// invokes a remote method by name. `Drop` sends a destroy request to the
/// server (`rmi_proxy_release`).
pub struct Proxy {
    handle: sys::rmi_proxy_handle,
    // Raw pointers to `Box<Box<dyn FnMut(&Dynamic) + Send>>` registered via
    // `on_event`, freed on `Drop` (mirrors `Dynamic::callbacks`).
    callbacks: Vec<*mut c_void>,
}

unsafe impl Send for Proxy {}

impl Proxy {
    pub(crate) fn from_raw(handle: sys::rmi_proxy_handle) -> Self {
        Proxy {
            handle,
            callbacks: Vec::new(),
        }
    }

    // ── Remote field access ─────────────────────────────────────────────

    /// Fetches a full snapshot, or a projected subset of fields.
    pub fn get(&self, projection: Option<&Dynamic>, timeout_ms: i64) -> Result<Dynamic, RmiError> {
        let mut out: sys::bison_handle = ptr::null_mut();
        let rc = unsafe {
            sys::rmi_proxy_get(self.handle, params_handle(projection), &mut out, timeout_ms)
        };
        check(rc, "proxy.get")?;
        Ok(Dynamic::from_raw(out, true))
    }

    pub fn get_async(&self, projection: Option<&Dynamic>) -> Result<Future, RmiError> {
        let mut out: sys::rmi_future_handle = ptr::null_mut();
        let rc =
            unsafe { sys::rmi_proxy_get_async(self.handle, params_handle(projection), &mut out) };
        check(rc, "proxy.get_async")?;
        Ok(Future::from_raw(out))
    }

    /// Applies a partial field update without resetting unspecified fields.
    pub fn set(&self, fields: &Dynamic, timeout_ms: i64) -> Result<(), RmiError> {
        let rc = unsafe { sys::rmi_proxy_set(self.handle, fields.raw_handle(), timeout_ms) };
        check(rc, "proxy.set")
    }

    pub fn set_async(&self, fields: &Dynamic) -> Result<Future, RmiError> {
        let mut out: sys::rmi_future_handle = ptr::null_mut();
        let rc = unsafe { sys::rmi_proxy_set_async(self.handle, fields.raw_handle(), &mut out) };
        check(rc, "proxy.set_async")?;
        Ok(Future::from_raw(out))
    }

    /// Resets explicitly-set fields back to prototype/inherited defaults.
    pub fn clear(&self, timeout_ms: i64) -> Result<(), RmiError> {
        let rc = unsafe { sys::rmi_proxy_clear(self.handle, timeout_ms) };
        check(rc, "proxy.clear")
    }

    pub fn clear_async(&self) -> Result<Future, RmiError> {
        let mut out: sys::rmi_future_handle = ptr::null_mut();
        let rc = unsafe { sys::rmi_proxy_clear_async(self.handle, &mut out) };
        check(rc, "proxy.clear_async")?;
        Ok(Future::from_raw(out))
    }

    // ── Remote method calls ─────────────────────────────────────────────

    /// Invokes a named remote method with `params`.
    pub fn call(
        &self,
        name: &str,
        params: Option<&Dynamic>,
        timeout_ms: i64,
    ) -> Result<Dynamic, RmiError> {
        let mut out: sys::bison_handle = ptr::null_mut();
        let rc = unsafe {
            sys::rmi_proxy_call(
                self.handle,
                key(name),
                params_handle(params),
                &mut out,
                timeout_ms,
            )
        };
        check(rc, &format!("proxy.call({name:?})"))?;
        Ok(Dynamic::from_raw(out, true))
    }

    pub fn call_async(&self, name: &str, params: Option<&Dynamic>) -> Result<Future, RmiError> {
        let mut out: sys::rmi_future_handle = ptr::null_mut();
        let rc = unsafe {
            sys::rmi_proxy_call_async(self.handle, key(name), params_handle(params), &mut out)
        };
        check(rc, &format!("proxy.call_async({name:?})"))?;
        Ok(Future::from_raw(out))
    }

    // ── Events ───────────────────────────────────────────────────────────

    /// Subscribes to a server-initiated event on this object. `handler` must
    /// be `Send`: it is invoked from whichever client worker thread receives
    /// the push event, not necessarily the thread that called `on_event`.
    pub fn on_event<F>(&mut self, name: &str, handler: F) -> Result<(), RmiError>
    where
        F: FnMut(&Dynamic) + Send + 'static,
    {
        let boxed: Box<EventCallback> = Box::new(handler);
        let raw = Box::into_raw(Box::new(boxed)) as *mut c_void;
        let rc = unsafe { sys::rmi_proxy_on_event(self.handle, key(name), event_trampoline, raw) };
        if rc != sys::RMI_OK {
            unsafe { drop(Box::from_raw(raw as *mut Box<EventCallback>)) };
            return Err(RmiError::new(rc, &format!("on_event({name:?})")));
        }
        self.callbacks.push(raw);
        Ok(())
    }
}

impl Drop for Proxy {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            unsafe { sys::rmi_proxy_release(self.handle) };
            self.handle = ptr::null_mut();
        }
        for raw in self.callbacks.drain(..) {
            unsafe { drop(Box::from_raw(raw as *mut Box<EventCallback>)) };
        }
    }
}

unsafe extern "C" fn event_trampoline(params_h: sys::bison_handle, user: *mut c_void) {
    let closure = &mut *(user as *mut Box<EventCallback>);
    let params_dyn = Dynamic::from_raw(params_h, false);
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        closure(&params_dyn);
    }));
}

// ─── Client ─────────────────────────────────────────────────────────────────

/// RAII wrapper around an `rmi_client_handle`. Construct via [`Client::tcp`],
/// [`Client::tls`], [`Client::pipe`], [`Client::term`], or
/// [`Client::standalone`].
pub struct Client {
    handle: sys::rmi_client_handle,
    connected: bool,
}

unsafe impl Send for Client {}

impl Client {
    fn from_raw(handle: sys::rmi_client_handle) -> Self {
        Client {
            handle,
            connected: false,
        }
    }

    pub fn tcp(host: &str, port: u16) -> Client {
        let c = CString::new(host).expect("host must not contain a NUL byte");
        let h = unsafe { sys::rmi_client_tcp_create(c.as_ptr(), port) };
        assert!(!h.is_null(), "rmi_client_tcp_create failed");
        Client::from_raw(h)
    }

    /// Creates a TLS-secured TCP client (not yet connected). TLS
    /// trust/identity material (`ca_file`/`ca_pem`, `insecure_skip_verify`,
    /// `cert_file`/`cert_pem`, `key_file`/`key_pem`, `key_password`,
    /// `server_name`) is supplied via [`Client::connect`]'s `params`.
    pub fn tls(host: &str, port: u16) -> Client {
        let c = CString::new(host).expect("host must not contain a NUL byte");
        let h = unsafe { sys::rmi_client_tls_create(c.as_ptr(), port) };
        assert!(!h.is_null(), "rmi_client_tls_create failed");
        Client::from_raw(h)
    }

    pub fn pipe(path: &str) -> Client {
        let c = CString::new(path).expect("path must not contain a NUL byte");
        let h = unsafe { sys::rmi_client_pipe_create(c.as_ptr()) };
        assert!(!h.is_null(), "rmi_client_pipe_create failed");
        Client::from_raw(h)
    }

    pub fn term() -> Client {
        let h = unsafe { sys::rmi_client_term_create() };
        assert!(!h.is_null(), "rmi_client_term_create failed");
        Client::from_raw(h)
    }

    /// In-process client dispatching directly to the local class registry.
    pub fn standalone() -> Client {
        let h = unsafe { sys::rmi_standalone_create() };
        assert!(!h.is_null(), "rmi_standalone_create failed");
        Client::from_raw(h)
    }

    pub fn connect(&mut self, params: Option<&Dynamic>) -> Result<(), RmiError> {
        let rc = unsafe { sys::rmi_client_connect(self.handle, params_handle(params)) };
        check(rc, "connect")?;
        self.connected = true;
        Ok(())
    }

    pub fn disconnect(&mut self) -> Result<(), RmiError> {
        if self.connected {
            let rc = unsafe { sys::rmi_client_disconnect(self.handle) };
            check(rc, "disconnect")?;
            self.connected = false;
        }
        Ok(())
    }

    /// Instantiates a remote object and returns a [`Proxy`] to it.
    pub fn instantiate(
        &self,
        klass_name: &str,
        ns_name: &str,
        params: Option<&Dynamic>,
    ) -> Result<Proxy, RmiError> {
        let mut out: sys::rmi_proxy_handle = ptr::null_mut();
        let rc = unsafe {
            sys::rmi_client_instantiate(
                self.handle,
                key_or_zero(ns_name),
                key(klass_name),
                params_handle(params),
                &mut out,
            )
        };
        check(rc, &format!("instantiate({klass_name:?})"))?;
        Ok(Proxy::from_raw(out))
    }

    pub fn instantiate_async(
        &self,
        klass_name: &str,
        ns_name: &str,
        params: Option<&Dynamic>,
    ) -> Result<Future, RmiError> {
        let mut out: sys::rmi_future_handle = ptr::null_mut();
        let rc = unsafe {
            sys::rmi_client_instantiate_async(
                self.handle,
                key_or_zero(ns_name),
                key(klass_name),
                params_handle(params),
                &mut out,
            )
        };
        check(rc, &format!("instantiate_async({klass_name:?})"))?;
        Ok(Future::from_raw(out))
    }

    /// Fetches class metadata. An empty `klass_name` queries all metadata.
    pub fn describe(&self, klass_name: &str, ns_name: &str) -> Result<Dynamic, RmiError> {
        let mut out: sys::bison_handle = ptr::null_mut();
        let rc = unsafe {
            sys::rmi_client_describe(
                self.handle,
                key_or_zero(ns_name),
                key_or_zero(klass_name),
                &mut out,
            )
        };
        check(rc, "describe")?;
        Ok(Dynamic::from_raw(out, true))
    }

    pub fn describe_async(&self, klass_name: &str, ns_name: &str) -> Result<Future, RmiError> {
        let mut out: sys::rmi_future_handle = ptr::null_mut();
        let rc = unsafe {
            sys::rmi_client_describe_async(
                self.handle,
                key_or_zero(ns_name),
                key_or_zero(klass_name),
                &mut out,
            )
        };
        check(rc, "describe_async")?;
        Ok(Future::from_raw(out))
    }
}

impl Drop for Client {
    fn drop(&mut self) {
        if self.connected {
            unsafe { sys::rmi_client_disconnect(self.handle) };
            self.connected = false;
        }
        if !self.handle.is_null() {
            unsafe { sys::rmi_client_release(self.handle) };
            self.handle = ptr::null_mut();
        }
    }
}

// ─── Server ─────────────────────────────────────────────────────────────────

type AuthCallback = dyn FnMut(&Dynamic) -> (bool, String) + Send;

/// RAII wrapper around an `rmi_server_handle`. Construct via [`Server::tcp`],
/// [`Server::tls`], [`Server::pipe`], or [`Server::term`].
pub struct Server {
    handle: sys::rmi_server_handle,
    listening: bool,
    // Raw pointer to the boxed auth closure registered via `listen`, if any
    // (mirrors `Dynamic::callbacks`); freed on `Drop`.
    callbacks: Vec<*mut c_void>,
}

unsafe impl Send for Server {}

impl Server {
    fn from_raw(handle: sys::rmi_server_handle) -> Self {
        Server {
            handle,
            listening: false,
            callbacks: Vec::new(),
        }
    }

    pub fn tcp(host: &str, port: u16) -> Server {
        let c = CString::new(host).expect("host must not contain a NUL byte");
        let h = unsafe { sys::rmi_server_tcp_create(c.as_ptr(), port) };
        assert!(!h.is_null(), "rmi_server_tcp_create failed");
        Server::from_raw(h)
    }

    /// Creates a TLS-secured TCP server listener (not yet listening). Server
    /// certificate/key material (`cert_file`/`cert_pem`, `key_file`/`key_pem`,
    /// `key_password`) and optional mutual-TLS settings (`client_auth`,
    /// `ca_file`/`ca_pem`) are supplied via [`Server::listen`]'s `params`.
    pub fn tls(host: &str, port: u16) -> Server {
        let c = CString::new(host).expect("host must not contain a NUL byte");
        let h = unsafe { sys::rmi_server_tls_create(c.as_ptr(), port) };
        assert!(!h.is_null(), "rmi_server_tls_create failed");
        Server::from_raw(h)
    }

    pub fn pipe(path: &str) -> Server {
        let c = CString::new(path).expect("path must not contain a NUL byte");
        let h = unsafe { sys::rmi_server_pipe_create(c.as_ptr()) };
        assert!(!h.is_null(), "rmi_server_pipe_create failed");
        Server::from_raw(h)
    }

    pub fn term(cmd: Option<&str>) -> Server {
        let c = cmd.map(|s| CString::new(s).expect("cmd must not contain a NUL byte"));
        let p = c.as_deref().map_or(ptr::null(), |c| c.as_ptr());
        let h = unsafe { sys::rmi_server_term_create(p) };
        assert!(!h.is_null(), "rmi_server_term_create failed");
        Server::from_raw(h)
    }

    /// Starts accepting client connections and spawns background worker
    /// threads.
    ///
    /// `auth`, if given, is evaluated once per incoming connection for as
    /// long as the server keeps listening -- it can only be set here, not
    /// changed afterward. It receives the client's `OP_CONNECT` payload and
    /// returns `(accepted, identity)`. Identity strings longer than 255
    /// UTF-8 bytes are truncated (the C ABI uses a fixed 256-byte buffer).
    pub fn listen<F>(&mut self, params: Option<&Dynamic>, auth: Option<F>) -> Result<(), RmiError>
    where
        F: FnMut(&Dynamic) -> (bool, String) + Send + 'static,
    {
        let (auth_fn, auth_user): (Option<sys::rmi_auth_fn>, *mut c_void) = match auth {
            None => (None, ptr::null_mut()),
            Some(cb) => {
                let boxed: Box<AuthCallback> = Box::new(cb);
                let raw = Box::into_raw(Box::new(boxed)) as *mut c_void;
                self.callbacks.push(raw);
                (Some(auth_trampoline as sys::rmi_auth_fn), raw)
            }
        };
        let rc = unsafe {
            sys::rmi_server_listen(self.handle, params_handle(params), auth_fn, auth_user)
        };
        check(rc, "listen")?;
        self.listening = true;
        Ok(())
    }

    pub fn stop(&mut self) {
        if self.listening {
            unsafe { sys::rmi_server_stop(self.handle) };
            self.listening = false;
        }
    }

    // ── Profiling ────────────────────────────────────────────────────────

    /// Opts this server into Perfetto trace capture. Must be called before
    /// [`Server::start_capture`].
    pub fn enable_profiling(&self, output_dir: &str) -> Result<(), RmiError> {
        let c = CString::new(output_dir).map_err(|_| {
            RmiError::new(
                sys::RMI_ERR_EXCEPTION,
                "enable_profiling: output_dir contains a NUL byte",
            )
        })?;
        let rc = unsafe { sys::rmi_server_enable_profiling(self.handle, c.as_ptr()) };
        check(rc, "enable_profiling")
    }

    /// Starts Perfetto capture. Returns whether capture is active afterward
    /// (`false` if [`Server::enable_profiling`] was never called).
    pub fn start_capture(&self, label: Option<&str>) -> Result<bool, RmiError> {
        let c = label.map(|s| CString::new(s).unwrap_or_default());
        let p = c.as_deref().map_or(ptr::null(), |c| c.as_ptr());
        let mut started = false;
        let rc = unsafe { sys::rmi_server_start_capture(self.handle, p, &mut started) };
        check(rc, "start_capture")?;
        Ok(started)
    }

    /// Stops Perfetto capture and finalizes the trace file.
    pub fn stop_capture(&self) -> Result<(), RmiError> {
        let rc = unsafe { sys::rmi_server_stop_capture(self.handle) };
        check(rc, "stop_capture")
    }

    /// Queries whether this server currently has Perfetto capture active.
    pub fn is_capture_active(&self) -> Result<bool, RmiError> {
        let mut active = false;
        let rc = unsafe { sys::rmi_server_is_capture_active(self.handle, &mut active) };
        check(rc, "is_capture_active")?;
        Ok(active)
    }
}

impl Drop for Server {
    fn drop(&mut self) {
        self.stop();
        if !self.handle.is_null() {
            unsafe { sys::rmi_server_release(self.handle) };
            self.handle = ptr::null_mut();
        }
        for raw in self.callbacks.drain(..) {
            unsafe { drop(Box::from_raw(raw as *mut Box<AuthCallback>)) };
        }
    }
}

unsafe extern "C" fn auth_trampoline(
    payload: sys::bison_handle,
    identity_buf: *mut c_char,
    identity_buf_len: usize,
    user: *mut c_void,
) -> bool {
    let closure = &mut *(user as *mut Box<AuthCallback>);
    let payload_dyn = Dynamic::from_raw(payload, false);
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| closure(&payload_dyn)));
    match result {
        Ok((accepted, identity)) => {
            if accepted && !identity.is_empty() && identity_buf_len > 0 {
                let bytes = identity.as_bytes();
                let n = bytes.len().min(identity_buf_len - 1);
                ptr::copy_nonoverlapping(bytes.as_ptr(), identity_buf as *mut u8, n);
                *identity_buf.add(n) = 0;
            }
            accepted
        }
        Err(_) => false,
    }
}

// ─── Profiling: free-function trace calls ──────────────────────────────────
//
// `rmi_trace_scope_begin()`/`rmi_trace_instant()`/`rmi_trace_counter_*()`
// take a `const char*` the library does not copy on the hot path -- the
// header requires the pointer to outlive the traced slice/event. Since
// these free functions take an ordinary `&str`, each unique name's `CString`
// is interned in a process-wide cache (never evicted) so the pointer handed
// to the C library stays valid indefinitely, rather than dangling the
// instant a temporary `CString` were dropped after the call returned.

fn trace_name_ptr(name: &str) -> *const c_char {
    static CACHE: OnceLock<Mutex<HashMap<String, CString>>> = OnceLock::new();
    let cache = CACHE.get_or_init(|| Mutex::new(HashMap::new()));
    let mut map = cache.lock().unwrap();
    if let Some(c) = map.get(name) {
        return c.as_ptr();
    }
    let c = CString::new(name).unwrap_or_default();
    let ptr = c.as_ptr();
    map.insert(name.to_string(), c);
    ptr
}

/// Begins a named trace slice on the calling thread. Every call must be
/// matched by a later [`trace_scope_end`] on the same thread. No-op if no
/// server or client in this process currently has an active profiling
/// recorder.
pub fn trace_scope_begin(name: &str) {
    unsafe { sys::rmi_trace_scope_begin(trace_name_ptr(name)) };
}

/// Ends the most recently begun trace slice on the calling thread.
pub fn trace_scope_end() {
    unsafe { sys::rmi_trace_scope_end() };
}

/// Records a zero-duration named instant event on the calling thread.
pub fn trace_instant(name: &str) {
    unsafe { sys::rmi_trace_instant(trace_name_ptr(name)) };
}

/// Records an integer-valued sample on a named counter track.
pub fn trace_counter_int(name: &str, value: i64) {
    unsafe { sys::rmi_trace_counter_int(trace_name_ptr(name), value) };
}

/// Records a floating-point sample on a named counter track.
pub fn trace_counter_double(name: &str, value: f64) {
    unsafe { sys::rmi_trace_counter_double(trace_name_ptr(name), value) };
}

/// Checks whether this process currently has an active profiling recorder
/// that trace calls right now will actually be recorded to.
pub fn trace_is_active() -> bool {
    unsafe { sys::rmi_trace_is_active() }
}

// MIT License © 2025 Binary Dice Games
//! Hand-maintained `extern "C"` mirror of `bison_c.h` and `rmi_c.h`.
//!
//! This module is the Rust analogue of `bison/_native.py`'s
//! `_setup_signatures()` / C#'s `Native.cs`: every exported `bison_*` /
//! `rmi_*` C function gets one raw declaration here, grouped by the same
//! `═══` section dividers the headers themselves use. No `bindgen` codegen
//! is used, for consistency with the other bindings (none of which use a
//! header-codegen tool) and to avoid a new build-dependency on libclang.
//!
//! This is a private, unsafe layer; public consumers use
//! [`crate::dynamic`] and [`crate::rmi`] instead.

#![allow(non_camel_case_types)]

use std::os::raw::{c_char, c_int, c_void};

// ─── Shared C type aliases ──────────────────────────────────────────────────

pub type bison_handle = *mut c_void;
pub type bison_hash = u32;
pub type bison_error = c_int;

pub type rmi_client_handle = *mut c_void;
pub type rmi_server_handle = *mut c_void;
pub type rmi_proxy_handle = *mut c_void;
pub type rmi_future_handle = *mut c_void;
pub type rmi_error = c_int;

// ─── bison_error codes ──────────────────────────────────────────────────────

pub const BISON_OK: bison_error = 0;
pub const BISON_ERR_NULL: bison_error = -1;
pub const BISON_ERR_TYPE: bison_error = -2;
pub const BISON_ERR_NOT_FOUND: bison_error = -3;
pub const BISON_ERR_DUPLICATE: bison_error = -4;
pub const BISON_ERR_EXCEPTION: bison_error = -5;
pub const BISON_ERR_PARSE: bison_error = -6;

// ─── rmi_error codes ────────────────────────────────────────────────────────

pub const RMI_OK: rmi_error = 0;
pub const RMI_ERR_NULL: rmi_error = -1;
pub const RMI_ERR_INVALID_STATE: rmi_error = -2;
pub const RMI_ERR_TIMEOUT: rmi_error = -3;
pub const RMI_ERR_REMOTE_EXCEPTION: rmi_error = -4;
pub const RMI_ERR_TRANSPORT: rmi_error = -5;
pub const RMI_ERR_EXCEPTION: rmi_error = -6;

// ─── Struct types ───────────────────────────────────────────────────────────

/// Mirrors `bison_attributes`.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct bison_attributes {
    pub display_name: *const c_char,
    pub description: *const c_char,
    pub category: *const c_char,
    pub obsolete: c_int,
    pub obsolete_message: *const c_char,
    pub required: c_int,
}

impl Default for bison_attributes {
    fn default() -> Self {
        bison_attributes {
            display_name: std::ptr::null(),
            description: std::ptr::null(),
            category: std::ptr::null(),
            obsolete: 0,
            obsolete_message: std::ptr::null(),
            required: 0,
        }
    }
}

/// Mirrors `bison_print_options`.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct bison_print_options {
    pub multiline: c_int,
    pub indent: *const c_char,
}

// ─── Callback types ─────────────────────────────────────────────────────────

/// `bison_method_fn`: `void (*)(bison_handle self, bison_handle params, bison_handle result, void* user)`
pub type bison_method_fn = unsafe extern "C" fn(
    self_: bison_handle,
    params: bison_handle,
    result: bison_handle,
    user: *mut c_void,
);

/// `rmi_proxy_event_fn`: `void (*)(bison_handle params, void* user)`
pub type rmi_proxy_event_fn = unsafe extern "C" fn(params: bison_handle, user: *mut c_void);

/// `rmi_auth_fn`: `bool (*)(bison_handle payload, char* identity_buf, size_t identity_buf_len, void* user)`
pub type rmi_auth_fn = unsafe extern "C" fn(
    payload: bison_handle,
    identity_buf: *mut c_char,
    identity_buf_len: usize,
    user: *mut c_void,
) -> bool;

extern "C" {
    // ── bison_c.h: lifecycle ────────────────────────────────────────────────

    pub fn bison_create(klass_name: bison_hash) -> bison_handle;
    pub fn bison_instantiate(ns_name: bison_hash, klass_name: bison_hash) -> bison_handle;
    pub fn bison_add_ref(h: bison_handle) -> bison_handle;
    pub fn bison_release(h: bison_handle);
    pub fn bison_clone(h: bison_handle) -> bison_handle;

    // ── Import helpers ──────────────────────────────────────────────────────

    pub fn bison_from_json(json: *const c_char) -> bison_handle;
    pub fn bison_from_yaml(yaml: *const c_char) -> bison_handle;

    // ── Export helpers ──────────────────────────────────────────────────────

    pub fn bison_to_json(h: bison_handle, indent: c_int, out: *mut *mut c_char) -> bison_error;
    pub fn bison_to_yaml(h: bison_handle, out: *mut *mut c_char) -> bison_error;

    // ── Pretty-print ─────────────────────────────────────────────────────────

    pub fn bison_print(
        h: bison_handle,
        opts: *const bison_print_options,
        out: *mut *mut c_char,
    ) -> bison_error;
    pub fn bison_free_string(s: *mut c_char);

    // ── Class registry ──────────────────────────────────────────────────────

    pub fn bison_add_class(
        ns_name: bison_hash,
        klass: bison_handle,
        parent_name: bison_hash,
        meta: *const bison_attributes,
    ) -> bison_error;
    pub fn bison_find_class(ns_name: bison_hash, klass_name: bison_hash) -> bison_handle;
    pub fn bison_clear_registry();
    pub fn bison_get_class_attributes(
        ns_name: bison_hash,
        klass_name: bison_hash,
        out: *mut bison_attributes,
    ) -> bison_error;
    pub fn bison_get_field_attributes(
        h: bison_handle,
        field_key: bison_hash,
        out: *mut bison_attributes,
    ) -> bison_error;
    pub fn bison_get_method_attributes(
        h: bison_handle,
        method_key: bison_hash,
        out: *mut bison_attributes,
    ) -> bison_error;

    // ── Field access -- scalar setters (named) ──────────────────────────────

    pub fn bison_set_int(h: bison_handle, name: bison_hash, value: i32) -> bison_error;
    pub fn bison_set_float(h: bison_handle, name: bison_hash, value: f32) -> bison_error;
    pub fn bison_set_bool(h: bison_handle, name: bison_hash, value: c_int) -> bison_error;
    pub fn bison_set_string(h: bison_handle, name: bison_hash, value: *const c_char)
        -> bison_error;
    pub fn bison_set_key(h: bison_handle, name: bison_hash, value: bison_hash) -> bison_error;
    pub fn bison_set_object(h: bison_handle, name: bison_hash, value: bison_handle) -> bison_error;

    // ── Field access -- scalar setters (indexed) ────────────────────────────

    pub fn bison_set_int_at(h: bison_handle, index: usize, value: i32) -> bison_error;
    pub fn bison_set_float_at(h: bison_handle, index: usize, value: f32) -> bison_error;
    pub fn bison_set_string_at(h: bison_handle, index: usize, value: *const c_char) -> bison_error;
    pub fn bison_set_bool_at(h: bison_handle, index: usize, value: c_int) -> bison_error;
    pub fn bison_set_key_at(h: bison_handle, index: usize, value: bison_hash) -> bison_error;
    pub fn bison_set_object_at(h: bison_handle, index: usize, value: bison_handle) -> bison_error;

    // ── Field access -- scalar getters (named) ──────────────────────────────

    pub fn bison_get_int(h: bison_handle, name: bison_hash, out: *mut i32) -> bison_error;
    pub fn bison_get_float(h: bison_handle, name: bison_hash, out: *mut f32) -> bison_error;
    pub fn bison_get_bool(h: bison_handle, name: bison_hash, out: *mut c_int) -> bison_error;
    pub fn bison_get_string(
        h: bison_handle,
        name: bison_hash,
        buf: *mut c_char,
        buf_len: usize,
        len_out: *mut usize,
    ) -> bison_error;
    pub fn bison_get_object(
        h: bison_handle,
        name: bison_hash,
        out: *mut bison_handle,
    ) -> bison_error;
    pub fn bison_get_key(h: bison_handle, name: bison_hash, out: *mut bison_hash) -> bison_error;

    // ── Field access -- scalar getters (indexed) ────────────────────────────

    pub fn bison_get_int_at(h: bison_handle, index: usize, out: *mut i32) -> bison_error;
    pub fn bison_get_float_at(h: bison_handle, index: usize, out: *mut f32) -> bison_error;
    pub fn bison_get_string_at(
        h: bison_handle,
        index: usize,
        buf: *mut c_char,
        buf_len: usize,
        len_out: *mut usize,
    ) -> bison_error;
    pub fn bison_get_bool_at(h: bison_handle, index: usize, out: *mut c_int) -> bison_error;
    pub fn bison_get_key_at(h: bison_handle, index: usize, out: *mut bison_hash) -> bison_error;
    pub fn bison_get_object_at(
        h: bison_handle,
        index: usize,
        out: *mut bison_handle,
    ) -> bison_error;

    pub fn bison_size(h: bison_handle) -> usize;

    // ── Methods ──────────────────────────────────────────────────────────────

    pub fn bison_add_method(
        h: bison_handle,
        name: bison_hash,
        fn_: bison_method_fn,
        user: *mut c_void,
        meta: *const bison_attributes,
    ) -> bison_error;
    pub fn bison_call(
        h: bison_handle,
        name: bison_hash,
        params: bison_handle,
        result: *mut bison_handle,
    ) -> bison_error;

    // ── Field registration with optional attribute metadata ────────────────

    pub fn bison_add_field_int(
        obj: bison_handle,
        key: bison_hash,
        value: i32,
        meta: *const bison_attributes,
    ) -> bison_error;
    pub fn bison_add_field_float(
        obj: bison_handle,
        key: bison_hash,
        value: f32,
        meta: *const bison_attributes,
    ) -> bison_error;
    pub fn bison_add_field_bool(
        obj: bison_handle,
        key: bison_hash,
        value: c_int,
        meta: *const bison_attributes,
    ) -> bison_error;
    pub fn bison_add_field_string(
        obj: bison_handle,
        key: bison_hash,
        value: *const c_char,
        meta: *const bison_attributes,
    ) -> bison_error;
    pub fn bison_add_field_key(
        obj: bison_handle,
        key: bison_hash,
        value: bison_hash,
        meta: *const bison_attributes,
    ) -> bison_error;

    pub fn bison_add_field_vector_bool(
        obj: bison_handle,
        key: bison_hash,
        values: *const c_int,
        count: usize,
        meta: *const bison_attributes,
    ) -> bison_error;
    pub fn bison_add_field_vector_int(
        obj: bison_handle,
        key: bison_hash,
        values: *const i32,
        count: usize,
        meta: *const bison_attributes,
    ) -> bison_error;
    pub fn bison_add_field_vector_float(
        obj: bison_handle,
        key: bison_hash,
        values: *const f32,
        count: usize,
        meta: *const bison_attributes,
    ) -> bison_error;
    pub fn bison_add_field_vector_bytes(
        obj: bison_handle,
        key: bison_hash,
        values: *const u8,
        count: usize,
        meta: *const bison_attributes,
    ) -> bison_error;

    // ── Vector field access ─────────────────────────────────────────────────

    pub fn bison_get_vector_bool(
        h: bison_handle,
        name: bison_hash,
        buf: *mut c_int,
        buf_len: usize,
        len_out: *mut usize,
    ) -> bison_error;
    pub fn bison_get_vector_int(
        h: bison_handle,
        name: bison_hash,
        buf: *mut i32,
        buf_len: usize,
        len_out: *mut usize,
    ) -> bison_error;
    pub fn bison_get_vector_float(
        h: bison_handle,
        name: bison_hash,
        buf: *mut f32,
        buf_len: usize,
        len_out: *mut usize,
    ) -> bison_error;
    pub fn bison_get_vector_bytes(
        h: bison_handle,
        name: bison_hash,
        buf: *mut u8,
        buf_len: usize,
        len_out: *mut usize,
    ) -> bison_error;

    pub fn bison_set_vector_bool(
        h: bison_handle,
        name: bison_hash,
        values: *const c_int,
        count: usize,
    ) -> bison_error;
    pub fn bison_set_vector_int(
        h: bison_handle,
        name: bison_hash,
        values: *const i32,
        count: usize,
    ) -> bison_error;
    pub fn bison_set_vector_float(
        h: bison_handle,
        name: bison_hash,
        values: *const f32,
        count: usize,
    ) -> bison_error;
    pub fn bison_set_vector_bytes(
        h: bison_handle,
        name: bison_hash,
        values: *const u8,
        count: usize,
    ) -> bison_error;

    // ── Binary serialization ─────────────────────────────────────────────────

    pub fn bison_serialize(h: bison_handle, out: *mut *mut u8, out_len: *mut usize) -> bison_error;
    pub fn bison_deserialize(data: *const u8, len: usize, out: *mut bison_handle) -> bison_error;
    pub fn bison_free_buffer(buf: *mut u8);

    // ── Utility ──────────────────────────────────────────────────────────────

    pub fn bison_key(name: *const c_char) -> bison_hash;

    // ── rmi_c.h: futures ─────────────────────────────────────────────────────

    pub fn rmi_future_wait(future: rmi_future_handle, timeout_ms: i64) -> rmi_error;
    pub fn rmi_future_get_dynamic(
        future: *mut rmi_future_handle,
        out_value: *mut bison_handle,
    ) -> rmi_error;
    pub fn rmi_future_get_proxy(
        future: *mut rmi_future_handle,
        out_proxy: *mut rmi_proxy_handle,
    ) -> rmi_error;
    pub fn rmi_future_release(future: rmi_future_handle);

    // ── rmi_c.h: client ──────────────────────────────────────────────────────

    pub fn rmi_client_tcp_create(host: *const c_char, port: u16) -> rmi_client_handle;
    pub fn rmi_client_tls_create(host: *const c_char, port: u16) -> rmi_client_handle;
    pub fn rmi_client_pipe_create(path: *const c_char) -> rmi_client_handle;
    pub fn rmi_client_term_create() -> rmi_client_handle;
    pub fn rmi_standalone_create() -> rmi_client_handle;

    pub fn rmi_client_connect(client: rmi_client_handle, params: bison_handle) -> rmi_error;
    pub fn rmi_client_describe(
        client: rmi_client_handle,
        ns: bison_hash,
        klass: bison_hash,
        out_desc: *mut bison_handle,
    ) -> rmi_error;
    pub fn rmi_client_describe_async(
        client: rmi_client_handle,
        ns: bison_hash,
        klass: bison_hash,
        out_future: *mut rmi_future_handle,
    ) -> rmi_error;
    pub fn rmi_client_instantiate(
        client: rmi_client_handle,
        ns: bison_hash,
        klass: bison_hash,
        params: bison_handle,
        out_proxy: *mut rmi_proxy_handle,
    ) -> rmi_error;
    pub fn rmi_client_instantiate_async(
        client: rmi_client_handle,
        ns: bison_hash,
        klass: bison_hash,
        params: bison_handle,
        out_future: *mut rmi_future_handle,
    ) -> rmi_error;
    pub fn rmi_client_disconnect(client: rmi_client_handle) -> rmi_error;
    pub fn rmi_client_release(client: rmi_client_handle);

    // ── rmi_c.h: proxy ───────────────────────────────────────────────────────

    pub fn rmi_proxy_release(proxy: rmi_proxy_handle);
    pub fn rmi_proxy_on_event(
        proxy: rmi_proxy_handle,
        event_name: bison_hash,
        handler: rmi_proxy_event_fn,
        user: *mut c_void,
    ) -> rmi_error;
    pub fn rmi_proxy_clear(proxy: rmi_proxy_handle, timeout_ms: i64) -> rmi_error;
    pub fn rmi_proxy_clear_async(
        proxy: rmi_proxy_handle,
        out_future: *mut rmi_future_handle,
    ) -> rmi_error;
    pub fn rmi_proxy_set(
        proxy: rmi_proxy_handle,
        fields: bison_handle,
        timeout_ms: i64,
    ) -> rmi_error;
    pub fn rmi_proxy_set_async(
        proxy: rmi_proxy_handle,
        fields: bison_handle,
        out_future: *mut rmi_future_handle,
    ) -> rmi_error;
    pub fn rmi_proxy_get(
        proxy: rmi_proxy_handle,
        projection: bison_handle,
        out_result: *mut bison_handle,
        timeout_ms: i64,
    ) -> rmi_error;
    pub fn rmi_proxy_get_async(
        proxy: rmi_proxy_handle,
        projection: bison_handle,
        out_future: *mut rmi_future_handle,
    ) -> rmi_error;
    pub fn rmi_proxy_call(
        proxy: rmi_proxy_handle,
        method: bison_hash,
        params: bison_handle,
        out_result: *mut bison_handle,
        timeout_ms: i64,
    ) -> rmi_error;
    pub fn rmi_proxy_call_async(
        proxy: rmi_proxy_handle,
        method: bison_hash,
        params: bison_handle,
        out_future: *mut rmi_future_handle,
    ) -> rmi_error;

    // ── rmi_c.h: server ──────────────────────────────────────────────────────

    pub fn rmi_server_tcp_create(host: *const c_char, port: u16) -> rmi_server_handle;
    pub fn rmi_server_tls_create(host: *const c_char, port: u16) -> rmi_server_handle;
    pub fn rmi_server_pipe_create(path: *const c_char) -> rmi_server_handle;
    pub fn rmi_server_term_create(cmd: *const c_char) -> rmi_server_handle;

    pub fn rmi_server_listen(
        server: rmi_server_handle,
        params: bison_handle,
        auth_handler: Option<rmi_auth_fn>,
        auth_user: *mut c_void,
    ) -> rmi_error;
    pub fn rmi_server_stop(server: rmi_server_handle);
    pub fn rmi_server_release(server: rmi_server_handle);

    // ── rmi_c.h: profiling ──────────────────────────────────────────────────

    pub fn rmi_server_enable_profiling(
        server: rmi_server_handle,
        output_dir: *const c_char,
    ) -> rmi_error;
    pub fn rmi_server_start_capture(
        server: rmi_server_handle,
        label: *const c_char,
        out_started: *mut bool,
    ) -> rmi_error;
    pub fn rmi_server_stop_capture(server: rmi_server_handle) -> rmi_error;
    pub fn rmi_server_is_capture_active(
        server: rmi_server_handle,
        out_active: *mut bool,
    ) -> rmi_error;

    pub fn rmi_trace_scope_begin(name: *const c_char);
    pub fn rmi_trace_scope_end();
    pub fn rmi_trace_instant(name: *const c_char);
    pub fn rmi_trace_counter_int(name: *const c_char, value: i64);
    pub fn rmi_trace_counter_double(name: *const c_char, value: f64);
    pub fn rmi_trace_is_active() -> bool;
}

// MIT License © 2025 Binary Dice Games
//! Safe, idiomatic wrapper around `bison_c.h` -- the Rust analogue of
//! `bison/dynamic.py`'s `Dynamic` class and C#'s `Dynamic.cs`.
//!
//! [`Dynamic`] wraps an opaque `bison_handle` and exposes it through typed
//! getters/setters (`get_int`/`set_int`, ...) as well as an ergonomic,
//! dict-`__getitem__`-like [`Dynamic::get`]/[`Dynamic::set`] pair backed by
//! the [`Value`] enum. Reference counting is handled by [`Drop`]; callers
//! never need to call `bison_release` themselves.

use std::collections::HashMap;
use std::ffi::{CStr, CString};
use std::fmt;
use std::os::raw::{c_char, c_int, c_void};
use std::ptr;
use std::sync::{Mutex, OnceLock};

use crate::sys;

// ─── Name hashing ───────────────────────────────────────────────────────────

const KEY_CACHE_MAX: usize = 4096;

fn key_cache() -> &'static Mutex<HashMap<String, u32>> {
    static CACHE: OnceLock<Mutex<HashMap<String, u32>>> = OnceLock::new();
    CACHE.get_or_init(|| Mutex::new(HashMap::new()))
}

/// Returns the 32-bit FNV-1a hash of `name` (identical to `"name"_key` in
/// C++ and `bison.key(name)` in Python).
///
/// Rust, like Python and C#, has no way to hash a string literal before run
/// time (no `constexpr` equivalent reachable from this crate), so every
/// [`Dynamic`] field/method access funnels through this function. Results
/// are memoized in a cache bounded at [`KEY_CACHE_MAX`] entries -- the same
/// tradeoff `bison.dynamic.key()`'s `functools.lru_cache(maxsize=4096)` and
/// C#'s `Key.Of()` make: real callers draw field/method names from a small,
/// static, schema-defined set reused across many calls, so caching turns
/// most lookups into a hash-map hit instead of a string encode + FFI call,
/// while staying bounded so a caller hashing high-cardinality strings can't
/// grow it without bound.
pub fn key(name: &str) -> u32 {
    if let Some(&h) = key_cache().lock().unwrap().get(name) {
        return h;
    }
    let c = CString::new(name).expect("field/method names must not contain NUL bytes");
    let hash = unsafe { sys::bison_key(c.as_ptr()) };
    let mut cache = key_cache().lock().unwrap();
    if cache.len() < KEY_CACHE_MAX {
        cache.insert(name.to_string(), hash);
    }
    hash
}

pub(crate) fn key_or_zero(name: &str) -> u32 {
    if name.is_empty() {
        0
    } else {
        key(name)
    }
}

// ─── Errors ─────────────────────────────────────────────────────────────────

/// Raised when a `bison_*` C API call returns a non-zero error code.
#[derive(Debug, Clone)]
pub struct BisonError {
    /// The raw `bison_error` code (see `bison_c.h`).
    pub code: i32,
    message: String,
}

impl BisonError {
    fn new(code: i32, context: &str) -> Self {
        let msg = error_message(code);
        let message = if context.is_empty() {
            msg.to_string()
        } else {
            format!("{context}: {msg}")
        };
        BisonError { code, message }
    }
}

impl fmt::Display for BisonError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.message)
    }
}

impl std::error::Error for BisonError {}

fn error_message(code: i32) -> &'static str {
    match code {
        sys::BISON_ERR_NULL => "Null handle or pointer",
        sys::BISON_ERR_TYPE => "Field type mismatch",
        sys::BISON_ERR_NOT_FOUND => "Method or field not found",
        sys::BISON_ERR_DUPLICATE => "Duplicate class or method",
        sys::BISON_ERR_EXCEPTION => "Internal C++ exception",
        sys::BISON_ERR_PARSE => "Parse error (JSON / YAML / binary buffer)",
        _ => "Unknown error",
    }
}

fn check(rc: sys::bison_error, context: &str) -> Result<(), BisonError> {
    if rc == sys::BISON_OK {
        Ok(())
    } else {
        Err(BisonError::new(rc, context))
    }
}

// ─── Attributes ─────────────────────────────────────────────────────────────

/// Optional display/documentation metadata for a class, field, or method.
/// Mirrors `bison_attributes`.
#[derive(Debug, Clone, Default)]
pub struct Attributes {
    pub display_name: Option<String>,
    pub description: Option<String>,
    pub category: Option<String>,
    pub obsolete: bool,
    pub obsolete_message: Option<String>,
    pub required: bool,
}

/// Owns the `CString`s a `bison_attributes` C struct points into, so they
/// stay alive for the duration of the FFI call that uses them.
struct AttrCStrings {
    display_name: Option<CString>,
    description: Option<CString>,
    category: Option<CString>,
    obsolete_message: Option<CString>,
}

impl Attributes {
    fn to_c(&self) -> (sys::bison_attributes, AttrCStrings) {
        fn enc(s: &Option<String>) -> Option<CString> {
            s.as_deref().map(|s| CString::new(s).unwrap_or_default())
        }
        let strings = AttrCStrings {
            display_name: enc(&self.display_name),
            description: enc(&self.description),
            category: enc(&self.category),
            obsolete_message: enc(&self.obsolete_message),
        };
        let c = sys::bison_attributes {
            display_name: strings
                .display_name
                .as_ref()
                .map_or(ptr::null(), |c| c.as_ptr()),
            description: strings
                .description
                .as_ref()
                .map_or(ptr::null(), |c| c.as_ptr()),
            category: strings
                .category
                .as_ref()
                .map_or(ptr::null(), |c| c.as_ptr()),
            obsolete: self.obsolete as c_int,
            obsolete_message: strings
                .obsolete_message
                .as_ref()
                .map_or(ptr::null(), |c| c.as_ptr()),
            required: self.required as c_int,
        };
        (c, strings)
    }

    fn from_c(c: &sys::bison_attributes) -> Self {
        unsafe fn dec(p: *const c_char) -> Option<String> {
            if p.is_null() {
                None
            } else {
                Some(CStr::from_ptr(p).to_string_lossy().into_owned())
            }
        }
        Attributes {
            display_name: unsafe { dec(c.display_name) },
            description: unsafe { dec(c.description) },
            category: unsafe { dec(c.category) },
            obsolete: c.obsolete != 0,
            obsolete_message: unsafe { dec(c.obsolete_message) },
            required: c.required != 0,
        }
    }
}

/// Builds a `*const bison_attributes` from `meta` (or a null pointer for
/// `None`), keeping the backing `CString`s alive for the duration of `f`.
fn with_meta_ptr<R>(
    meta: Option<&Attributes>,
    f: impl FnOnce(*const sys::bison_attributes) -> R,
) -> R {
    match meta {
        None => f(ptr::null()),
        Some(m) => {
            let (c, _strings) = m.to_c();
            f(&c as *const sys::bison_attributes)
        }
    }
}

// ─── Value: ergonomic generic field access ─────────────────────────────────

/// A field value, for ergonomic (Python-`__getitem__`-like) generic access
/// via [`Dynamic::get`] / [`Dynamic::set`].
#[derive(Debug, Clone)]
pub enum Value {
    Int(i32),
    Float(f32),
    Bool(bool),
    Str(String),
    /// A `bison::key_t`-valued field (see [`Dynamic::get_key`]/[`Dynamic::set_key`]).
    Key(u32),
    /// A nested object field. `None` is a null object reference.
    Object(Option<Dynamic>),
    VecBool(Vec<bool>),
    VecInt(Vec<i32>),
    VecFloat(Vec<f32>),
    /// A `vector<uint8_t>` (raw byte buffer) field.
    Bytes(Vec<u8>),
}

impl From<i32> for Value {
    fn from(v: i32) -> Self {
        Value::Int(v)
    }
}
impl From<f32> for Value {
    fn from(v: f32) -> Self {
        Value::Float(v)
    }
}
impl From<bool> for Value {
    fn from(v: bool) -> Self {
        Value::Bool(v)
    }
}
impl From<&str> for Value {
    fn from(v: &str) -> Self {
        Value::Str(v.to_string())
    }
}
impl From<String> for Value {
    fn from(v: String) -> Self {
        Value::Str(v)
    }
}
impl From<Dynamic> for Value {
    fn from(v: Dynamic) -> Self {
        Value::Object(Some(v))
    }
}
impl From<Option<Dynamic>> for Value {
    fn from(v: Option<Dynamic>) -> Self {
        Value::Object(v)
    }
}
impl From<Vec<bool>> for Value {
    fn from(v: Vec<bool>) -> Self {
        Value::VecBool(v)
    }
}
impl From<Vec<i32>> for Value {
    fn from(v: Vec<i32>) -> Self {
        Value::VecInt(v)
    }
}
impl From<Vec<f32>> for Value {
    fn from(v: Vec<f32>) -> Self {
        Value::VecFloat(v)
    }
}
impl From<Vec<u8>> for Value {
    fn from(v: Vec<u8>) -> Self {
        Value::Bytes(v)
    }
}

// ─── Dynamic ────────────────────────────────────────────────────────────────

/// A reference-counted Bison dynamic object.
///
/// Wraps a `bison_handle`. `owned = true` (the common case) means this
/// instance holds a reference that [`Drop`] releases; `owned = false` marks
/// a non-owning view (`self`/`params`/`result` inside a method callback, or
/// a [`find_class`] lookup result) whose `Drop` is a no-op on the handle.
pub struct Dynamic {
    handle: sys::bison_handle,
    owned: bool,
    released: bool,
    // Raw pointers to `Box<Box<dyn FnMut(...) + Send>>` registered via
    // `add_method`, kept alive for as long as this `Dynamic` lives (mirrors
    // `bison.dynamic.Dynamic._callbacks` / C#'s `_callbacks` list) and freed
    // on `Drop`. See `add_method`'s doc comment for why a prototype
    // registered via `add_class` must be kept alive separately.
    callbacks: Vec<*mut c_void>,
}

// SAFETY: `Dynamic` owns (or non-owningly views) a single `bison_handle`.
// Moving it to another thread and using it there is sound under ordinary
// Rust ownership rules (only one thread has access to a given `Dynamic`
// value at a time); the C library's own thread-safety note in `bison_c.h`
// only warns against *concurrent, unsynchronized* access to the *same*
// handle from multiple threads, which safe Rust code already cannot do
// without `unsafe`. `Dynamic` is deliberately not `Sync`: shared (`&Dynamic`)
// access from multiple threads at once is not synchronized on the C++ side.
unsafe impl Send for Dynamic {}

type MethodCallback = dyn FnMut(&Dynamic, &Dynamic, &mut Dynamic) + Send;

impl Dynamic {
    /// Creates a new, empty object. Pass `""` for an anonymous object
    /// (equivalent to `Dynamic::default()`).
    pub fn new(klass_name: &str) -> Dynamic {
        let h = unsafe { sys::bison_create(key_or_zero(klass_name)) };
        assert!(!h.is_null(), "bison_create failed");
        Dynamic::from_raw(h, true)
    }

    pub(crate) fn from_raw(handle: sys::bison_handle, owned: bool) -> Dynamic {
        Dynamic {
            handle,
            owned,
            released: false,
            callbacks: Vec::new(),
        }
    }

    pub(crate) fn raw_handle(&self) -> sys::bison_handle {
        self.handle
    }

    // ── Lifecycle ────────────────────────────────────────────────────────

    /// Returns a new [`Dynamic`] sharing ownership of the same underlying
    /// object (`bison_add_ref`).
    pub fn add_ref(&self) -> Dynamic {
        let h = unsafe { sys::bison_add_ref(self.handle) };
        assert!(!h.is_null(), "bison_add_ref failed");
        Dynamic::from_raw(h, true)
    }

    // ── Field access -- scalar (named) ──────────────────────────────────

    pub fn get_int(&self, name: &str) -> Result<i32, BisonError> {
        let mut out: i32 = 0;
        let rc = unsafe { sys::bison_get_int(self.handle, key(name), &mut out) };
        check(rc, &format!("get_int[{name}]"))?;
        Ok(out)
    }

    pub fn set_int(&mut self, name: &str, value: i32) -> Result<(), BisonError> {
        let rc = unsafe { sys::bison_set_int(self.handle, key(name), value) };
        check(rc, &format!("set_int[{name}]"))
    }

    pub fn get_float(&self, name: &str) -> Result<f32, BisonError> {
        let mut out: f32 = 0.0;
        let rc = unsafe { sys::bison_get_float(self.handle, key(name), &mut out) };
        check(rc, &format!("get_float[{name}]"))?;
        Ok(out)
    }

    pub fn set_float(&mut self, name: &str, value: f32) -> Result<(), BisonError> {
        let rc = unsafe { sys::bison_set_float(self.handle, key(name), value) };
        check(rc, &format!("set_float[{name}]"))
    }

    pub fn get_bool(&self, name: &str) -> Result<bool, BisonError> {
        let mut out: c_int = 0;
        let rc = unsafe { sys::bison_get_bool(self.handle, key(name), &mut out) };
        check(rc, &format!("get_bool[{name}]"))?;
        Ok(out != 0)
    }

    pub fn set_bool(&mut self, name: &str, value: bool) -> Result<(), BisonError> {
        let rc = unsafe { sys::bison_set_bool(self.handle, key(name), value as c_int) };
        check(rc, &format!("set_bool[{name}]"))
    }

    pub fn get_string(&self, name: &str) -> Result<String, BisonError> {
        let k = key(name);
        let mut len_out: usize = 0;
        let rc = unsafe { sys::bison_get_string(self.handle, k, ptr::null_mut(), 0, &mut len_out) };
        check(rc, &format!("get_string[{name}]"))?;
        let mut buf = vec![0u8; len_out + 1];
        let rc2 = unsafe {
            sys::bison_get_string(
                self.handle,
                k,
                buf.as_mut_ptr() as *mut c_char,
                buf.len(),
                ptr::null_mut(),
            )
        };
        check(rc2, &format!("get_string[{name}]"))?;
        buf.truncate(len_out);
        Ok(String::from_utf8_lossy(&buf).into_owned())
    }

    pub fn set_string(&mut self, name: &str, value: &str) -> Result<(), BisonError> {
        let c = CString::new(value).map_err(|_| {
            BisonError::new(
                sys::BISON_ERR_EXCEPTION,
                "set_string: value contains a NUL byte",
            )
        })?;
        let rc = unsafe { sys::bison_set_string(self.handle, key(name), c.as_ptr()) };
        check(rc, &format!("set_string[{name}]"))
    }

    /// Reads a `bison::key_t`-valued field (e.g. an object's `"id"`, or an
    /// enum-like selector). See the module doc for why this is a distinct
    /// field-variant type from `int32_t`.
    pub fn get_key(&self, name: &str) -> Result<u32, BisonError> {
        let mut out: u32 = 0;
        let rc = unsafe { sys::bison_get_key(self.handle, key(name), &mut out) };
        check(rc, &format!("get_key[{name}]"))?;
        Ok(out)
    }

    /// Sets a `bison::key_t`-valued field. `value` is an already-hashed key
    /// (e.g. from [`key`]) -- unlike [`Dynamic::set_int`], a plain `i32`
    /// handed to `set` is always written as int32, since there is no way to
    /// tell "this int should become a key_t field" from context alone.
    pub fn set_key(&mut self, name: &str, value: u32) -> Result<(), BisonError> {
        let rc = unsafe { sys::bison_set_key(self.handle, key(name), value) };
        check(rc, &format!("set_key[{name}]"))
    }

    pub fn get_object(&self, name: &str) -> Result<Option<Dynamic>, BisonError> {
        let mut out: sys::bison_handle = ptr::null_mut();
        let rc = unsafe { sys::bison_get_object(self.handle, key(name), &mut out) };
        check(rc, &format!("get_object[{name}]"))?;
        Ok(if out.is_null() {
            None
        } else {
            Some(Dynamic::from_raw(out, true))
        })
    }

    /// Sets a nested object field. The library increments `value`'s
    /// ref-count, so `value` remains independently owned by the caller.
    pub fn set_object(&mut self, name: &str, value: Option<&Dynamic>) -> Result<(), BisonError> {
        let h = value.map_or(ptr::null_mut(), |d| d.handle);
        let rc = unsafe { sys::bison_set_object(self.handle, key(name), h) };
        check(rc, &format!("set_object[{name}]"))
    }

    // ── Field access -- scalar (indexed) ────────────────────────────────

    pub fn get_int_at(&self, index: usize) -> Result<i32, BisonError> {
        let mut out: i32 = 0;
        let rc = unsafe { sys::bison_get_int_at(self.handle, index, &mut out) };
        check(rc, &format!("get_int_at[{index}]"))?;
        Ok(out)
    }

    pub fn set_int_at(&mut self, index: usize, value: i32) -> Result<(), BisonError> {
        let rc = unsafe { sys::bison_set_int_at(self.handle, index, value) };
        check(rc, &format!("set_int_at[{index}]"))
    }

    pub fn get_float_at(&self, index: usize) -> Result<f32, BisonError> {
        let mut out: f32 = 0.0;
        let rc = unsafe { sys::bison_get_float_at(self.handle, index, &mut out) };
        check(rc, &format!("get_float_at[{index}]"))?;
        Ok(out)
    }

    pub fn set_float_at(&mut self, index: usize, value: f32) -> Result<(), BisonError> {
        let rc = unsafe { sys::bison_set_float_at(self.handle, index, value) };
        check(rc, &format!("set_float_at[{index}]"))
    }

    pub fn get_bool_at(&self, index: usize) -> Result<bool, BisonError> {
        let mut out: c_int = 0;
        let rc = unsafe { sys::bison_get_bool_at(self.handle, index, &mut out) };
        check(rc, &format!("get_bool_at[{index}]"))?;
        Ok(out != 0)
    }

    pub fn set_bool_at(&mut self, index: usize, value: bool) -> Result<(), BisonError> {
        let rc = unsafe { sys::bison_set_bool_at(self.handle, index, value as c_int) };
        check(rc, &format!("set_bool_at[{index}]"))
    }

    pub fn get_string_at(&self, index: usize) -> Result<String, BisonError> {
        let mut len_out: usize = 0;
        let rc = unsafe {
            sys::bison_get_string_at(self.handle, index, ptr::null_mut(), 0, &mut len_out)
        };
        check(rc, &format!("get_string_at[{index}]"))?;
        let mut buf = vec![0u8; len_out + 1];
        let rc2 = unsafe {
            sys::bison_get_string_at(
                self.handle,
                index,
                buf.as_mut_ptr() as *mut c_char,
                buf.len(),
                ptr::null_mut(),
            )
        };
        check(rc2, &format!("get_string_at[{index}]"))?;
        buf.truncate(len_out);
        Ok(String::from_utf8_lossy(&buf).into_owned())
    }

    pub fn set_string_at(&mut self, index: usize, value: &str) -> Result<(), BisonError> {
        let c = CString::new(value).map_err(|_| {
            BisonError::new(
                sys::BISON_ERR_EXCEPTION,
                "set_string_at: value contains a NUL byte",
            )
        })?;
        let rc = unsafe { sys::bison_set_string_at(self.handle, index, c.as_ptr()) };
        check(rc, &format!("set_string_at[{index}]"))
    }

    /// Indexed counterpart to [`Dynamic::get_key`].
    pub fn get_key_at(&self, index: usize) -> Result<u32, BisonError> {
        let mut out: u32 = 0;
        let rc = unsafe { sys::bison_get_key_at(self.handle, index, &mut out) };
        check(rc, &format!("get_key_at[{index}]"))?;
        Ok(out)
    }

    /// Indexed counterpart to [`Dynamic::set_key`].
    pub fn set_key_at(&mut self, index: usize, value: u32) -> Result<(), BisonError> {
        let rc = unsafe { sys::bison_set_key_at(self.handle, index, value) };
        check(rc, &format!("set_key_at[{index}]"))
    }

    pub fn get_object_at(&self, index: usize) -> Result<Option<Dynamic>, BisonError> {
        let mut out: sys::bison_handle = ptr::null_mut();
        let rc = unsafe { sys::bison_get_object_at(self.handle, index, &mut out) };
        check(rc, &format!("get_object_at[{index}]"))?;
        Ok(if out.is_null() {
            None
        } else {
            Some(Dynamic::from_raw(out, true))
        })
    }

    pub fn set_object_at(
        &mut self,
        index: usize,
        value: Option<&Dynamic>,
    ) -> Result<(), BisonError> {
        let h = value.map_or(ptr::null_mut(), |d| d.handle);
        let rc = unsafe { sys::bison_set_object_at(self.handle, index, h) };
        check(rc, &format!("set_object_at[{index}]"))
    }

    // ── Vector field access ─────────────────────────────────────────────

    pub fn get_vector_bool(&self, name: &str) -> Result<Vec<bool>, BisonError> {
        let k = key(name);
        let mut len_out: usize = 0;
        let rc =
            unsafe { sys::bison_get_vector_bool(self.handle, k, ptr::null_mut(), 0, &mut len_out) };
        check(rc, &format!("get_vector_bool[{name}]"))?;
        let mut buf = vec![0 as c_int; len_out];
        if len_out > 0 {
            let rc2 = unsafe {
                sys::bison_get_vector_bool(
                    self.handle,
                    k,
                    buf.as_mut_ptr(),
                    len_out,
                    ptr::null_mut(),
                )
            };
            check(rc2, &format!("get_vector_bool[{name}]"))?;
        }
        Ok(buf.into_iter().map(|v| v != 0).collect())
    }

    pub fn set_vector_bool(&mut self, name: &str, values: &[bool]) -> Result<(), BisonError> {
        let ints: Vec<c_int> = values.iter().map(|&b| b as c_int).collect();
        let p = if ints.is_empty() {
            ptr::null()
        } else {
            ints.as_ptr()
        };
        let rc = unsafe { sys::bison_set_vector_bool(self.handle, key(name), p, ints.len()) };
        check(rc, &format!("set_vector_bool[{name}]"))
    }

    pub fn add_field_vector_bool(
        &mut self,
        name: &str,
        values: &[bool],
        meta: Option<&Attributes>,
    ) -> Result<(), BisonError> {
        let k = key(name);
        let ints: Vec<c_int> = values.iter().map(|&b| b as c_int).collect();
        let p = if ints.is_empty() {
            ptr::null()
        } else {
            ints.as_ptr()
        };
        let rc = with_meta_ptr(meta, |m| unsafe {
            sys::bison_add_field_vector_bool(self.handle, k, p, ints.len(), m)
        });
        check(rc, &format!("add_field_vector_bool[{name}]"))
    }

    pub fn get_vector_int(&self, name: &str) -> Result<Vec<i32>, BisonError> {
        let k = key(name);
        let mut len_out: usize = 0;
        let rc =
            unsafe { sys::bison_get_vector_int(self.handle, k, ptr::null_mut(), 0, &mut len_out) };
        check(rc, &format!("get_vector_int[{name}]"))?;
        let mut buf = vec![0i32; len_out];
        if len_out > 0 {
            let rc2 = unsafe {
                sys::bison_get_vector_int(
                    self.handle,
                    k,
                    buf.as_mut_ptr(),
                    len_out,
                    ptr::null_mut(),
                )
            };
            check(rc2, &format!("get_vector_int[{name}]"))?;
        }
        Ok(buf)
    }

    pub fn set_vector_int(&mut self, name: &str, values: &[i32]) -> Result<(), BisonError> {
        let p = if values.is_empty() {
            ptr::null()
        } else {
            values.as_ptr()
        };
        let rc = unsafe { sys::bison_set_vector_int(self.handle, key(name), p, values.len()) };
        check(rc, &format!("set_vector_int[{name}]"))
    }

    pub fn add_field_vector_int(
        &mut self,
        name: &str,
        values: &[i32],
        meta: Option<&Attributes>,
    ) -> Result<(), BisonError> {
        let k = key(name);
        let p = if values.is_empty() {
            ptr::null()
        } else {
            values.as_ptr()
        };
        let rc = with_meta_ptr(meta, |m| unsafe {
            sys::bison_add_field_vector_int(self.handle, k, p, values.len(), m)
        });
        check(rc, &format!("add_field_vector_int[{name}]"))
    }

    pub fn get_vector_float(&self, name: &str) -> Result<Vec<f32>, BisonError> {
        let k = key(name);
        let mut len_out: usize = 0;
        let rc = unsafe {
            sys::bison_get_vector_float(self.handle, k, ptr::null_mut(), 0, &mut len_out)
        };
        check(rc, &format!("get_vector_float[{name}]"))?;
        let mut buf = vec![0f32; len_out];
        if len_out > 0 {
            let rc2 = unsafe {
                sys::bison_get_vector_float(
                    self.handle,
                    k,
                    buf.as_mut_ptr(),
                    len_out,
                    ptr::null_mut(),
                )
            };
            check(rc2, &format!("get_vector_float[{name}]"))?;
        }
        Ok(buf)
    }

    pub fn set_vector_float(&mut self, name: &str, values: &[f32]) -> Result<(), BisonError> {
        let p = if values.is_empty() {
            ptr::null()
        } else {
            values.as_ptr()
        };
        let rc = unsafe { sys::bison_set_vector_float(self.handle, key(name), p, values.len()) };
        check(rc, &format!("set_vector_float[{name}]"))
    }

    pub fn add_field_vector_float(
        &mut self,
        name: &str,
        values: &[f32],
        meta: Option<&Attributes>,
    ) -> Result<(), BisonError> {
        let k = key(name);
        let p = if values.is_empty() {
            ptr::null()
        } else {
            values.as_ptr()
        };
        let rc = with_meta_ptr(meta, |m| unsafe {
            sys::bison_add_field_vector_float(self.handle, k, p, values.len(), m)
        });
        check(rc, &format!("add_field_vector_float[{name}]"))
    }

    pub fn get_vector_bytes(&self, name: &str) -> Result<Vec<u8>, BisonError> {
        let k = key(name);
        let mut len_out: usize = 0;
        let rc = unsafe {
            sys::bison_get_vector_bytes(self.handle, k, ptr::null_mut(), 0, &mut len_out)
        };
        check(rc, &format!("get_vector_bytes[{name}]"))?;
        let mut buf = vec![0u8; len_out];
        if len_out > 0 {
            let rc2 = unsafe {
                sys::bison_get_vector_bytes(
                    self.handle,
                    k,
                    buf.as_mut_ptr(),
                    len_out,
                    ptr::null_mut(),
                )
            };
            check(rc2, &format!("get_vector_bytes[{name}]"))?;
        }
        Ok(buf)
    }

    pub fn set_vector_bytes(&mut self, name: &str, values: &[u8]) -> Result<(), BisonError> {
        let p = if values.is_empty() {
            ptr::null()
        } else {
            values.as_ptr()
        };
        let rc = unsafe { sys::bison_set_vector_bytes(self.handle, key(name), p, values.len()) };
        check(rc, &format!("set_vector_bytes[{name}]"))
    }

    pub fn add_field_vector_bytes(
        &mut self,
        name: &str,
        values: &[u8],
        meta: Option<&Attributes>,
    ) -> Result<(), BisonError> {
        let k = key(name);
        let p = if values.is_empty() {
            ptr::null()
        } else {
            values.as_ptr()
        };
        let rc = with_meta_ptr(meta, |m| unsafe {
            sys::bison_add_field_vector_bytes(self.handle, k, p, values.len(), m)
        });
        check(rc, &format!("add_field_vector_bytes[{name}]"))
    }

    // ── Ergonomic generic access (Value cascade) ────────────────────────

    /// Reads a field by name, probing each scalar/vector getter in turn
    /// (int -> float -> bool -> string -> object -> key -> vectors), the
    /// same cascade order `bison.dynamic.Dynamic.__getitem__` uses in the
    /// Python binding. An untouched field auto-vivifies as int32 zero (the
    /// underlying `dynamic::operator[]` semantics), so this only reaches a
    /// later branch once the field is already known to hold that type.
    pub fn get(&self, name: &str) -> Result<Value, BisonError> {
        if let Ok(v) = self.get_int(name) {
            return Ok(Value::Int(v));
        }
        if let Ok(v) = self.get_float(name) {
            return Ok(Value::Float(v));
        }
        if let Ok(v) = self.get_bool(name) {
            return Ok(Value::Bool(v));
        }
        if let Ok(v) = self.get_string(name) {
            return Ok(Value::Str(v));
        }
        if let Ok(v) = self.get_object(name) {
            return Ok(Value::Object(v));
        }
        if let Ok(v) = self.get_key(name) {
            return Ok(Value::Key(v));
        }
        if let Ok(v) = self.get_vector_int(name) {
            return Ok(Value::VecInt(v));
        }
        if let Ok(v) = self.get_vector_float(name) {
            return Ok(Value::VecFloat(v));
        }
        if let Ok(v) = self.get_vector_bool(name) {
            return Ok(Value::VecBool(v));
        }
        if let Ok(v) = self.get_vector_bytes(name) {
            return Ok(Value::Bytes(v));
        }
        Err(BisonError::new(
            sys::BISON_ERR_NOT_FOUND,
            &format!("get[{name}]: field not found or has an unsupported type"),
        ))
    }

    /// Writes a field by name, dispatching on the [`Value`] variant.
    /// Accepts anything with `Into<Value>` -- `d.set("hp", 100)`,
    /// `d.set("name", "hero")`, `d.set("active", true)` all work directly.
    pub fn set(&mut self, name: &str, value: impl Into<Value>) -> Result<(), BisonError> {
        match value.into() {
            Value::Int(v) => self.set_int(name, v),
            Value::Float(v) => self.set_float(name, v),
            Value::Bool(v) => self.set_bool(name, v),
            Value::Str(v) => self.set_string(name, &v),
            Value::Key(v) => self.set_key(name, v),
            Value::Object(v) => self.set_object(name, v.as_ref()),
            Value::VecBool(v) => self.set_vector_bool(name, &v),
            Value::VecInt(v) => self.set_vector_int(name, &v),
            Value::VecFloat(v) => self.set_vector_float(name, &v),
            Value::Bytes(v) => self.set_vector_bytes(name, &v),
        }
    }

    /// Indexed counterpart to [`Dynamic::get`] (int -> float -> bool ->
    /// string -> object -> key; `bison_c.h` has no indexed vector getters).
    pub fn get_at(&self, index: usize) -> Result<Value, BisonError> {
        if let Ok(v) = self.get_int_at(index) {
            return Ok(Value::Int(v));
        }
        if let Ok(v) = self.get_float_at(index) {
            return Ok(Value::Float(v));
        }
        if let Ok(v) = self.get_bool_at(index) {
            return Ok(Value::Bool(v));
        }
        if let Ok(v) = self.get_string_at(index) {
            return Ok(Value::Str(v));
        }
        if let Ok(v) = self.get_object_at(index) {
            return Ok(Value::Object(v));
        }
        if let Ok(v) = self.get_key_at(index) {
            return Ok(Value::Key(v));
        }
        Err(BisonError::new(
            sys::BISON_ERR_NOT_FOUND,
            &format!("get_at[{index}]: index not found or has an unsupported type"),
        ))
    }

    /// Indexed counterpart to [`Dynamic::set`]. Vector-typed values have no
    /// indexed form (`bison_c.h` exports no `bison_set_vector_*_at`) --
    /// only named fields support them.
    pub fn set_at(&mut self, index: usize, value: impl Into<Value>) -> Result<(), BisonError> {
        match value.into() {
            Value::Int(v) => self.set_int_at(index, v),
            Value::Float(v) => self.set_float_at(index, v),
            Value::Bool(v) => self.set_bool_at(index, v),
            Value::Str(v) => self.set_string_at(index, &v),
            Value::Key(v) => self.set_key_at(index, v),
            Value::Object(v) => self.set_object_at(index, v.as_ref()),
            Value::VecBool(_) | Value::VecInt(_) | Value::VecFloat(_) | Value::Bytes(_) => {
                Err(BisonError::new(
                    sys::BISON_ERR_TYPE,
                    &format!("set_at[{index}]: vector-typed values have no indexed form"),
                ))
            }
        }
    }

    // ── Array-like helpers ────────────────────────────────────────────────

    /// Number of array-like (numeric-key) elements.
    pub fn size(&self) -> usize {
        unsafe { sys::bison_size(self.handle) }
    }

    pub fn is_empty(&self) -> bool {
        self.size() == 0
    }

    pub fn iter(&self) -> DynamicIter<'_> {
        DynamicIter {
            dynamic: self,
            index: 0,
            len: self.size(),
        }
    }

    // ── Methods ──────────────────────────────────────────────────────────

    /// Registers a callback as a named method on this object.
    ///
    /// `callback` must be `Send` because it may be invoked from any
    /// worker thread that dispatches a call to this object (an RMI server
    /// worker thread for a remote call, in particular) -- not necessarily
    /// the thread that registered it.
    ///
    /// `bison_add_class()` copies field/method data out of a prototype into
    /// the C++ registry -- it does not take ownership of the handle -- but a
    /// registered method only copies the raw C function pointer, i.e. the
    /// trampoline behind this closure. That trampoline is only kept alive by
    /// this `Dynamic`'s own `callbacks` list; objects later instantiated
    /// from the class (`bison_instantiate`) call back into it directly. So a
    /// prototype registered via [`add_class`] must be kept alive for as long
    /// as its class stays in the registry -- `add_class` does this itself by
    /// moving the prototype into a process-wide keep-alive list (mirroring
    /// `bison.dynamic._registered_prototypes`).
    pub fn add_method<F>(&mut self, name: &str, callback: F) -> Result<(), BisonError>
    where
        F: FnMut(&Dynamic, &Dynamic, &mut Dynamic) + Send + 'static,
    {
        let k = key(name);
        let boxed: Box<MethodCallback> = Box::new(callback);
        let raw = Box::into_raw(Box::new(boxed)) as *mut c_void;
        let rc =
            unsafe { sys::bison_add_method(self.handle, k, method_trampoline, raw, ptr::null()) };
        if rc != sys::BISON_OK {
            // Registration failed: free the box we just allocated instead
            // of leaking it.
            unsafe { drop(Box::from_raw(raw as *mut Box<MethodCallback>)) };
            return Err(BisonError::new(rc, &format!("add_method({name:?})")));
        }
        self.callbacks.push(raw);
        Ok(())
    }

    /// Invokes a named method on this object and returns its result.
    pub fn call(&self, name: &str, params: &Dynamic) -> Result<Dynamic, BisonError> {
        let mut result: sys::bison_handle = ptr::null_mut();
        let rc = unsafe { sys::bison_call(self.handle, key(name), params.handle, &mut result) };
        check(rc, &format!("call({name:?})"))?;
        Ok(Dynamic::from_raw(result, true))
    }

    // ── Field / method attributes ────────────────────────────────────────

    pub fn field_attributes(&self, name: &str) -> Result<Attributes, BisonError> {
        let mut c = sys::bison_attributes::default();
        let rc = unsafe { sys::bison_get_field_attributes(self.handle, key(name), &mut c) };
        check(rc, &format!("field_attributes({name:?})"))?;
        Ok(Attributes::from_c(&c))
    }

    pub fn method_attributes(&self, name: &str) -> Result<Attributes, BisonError> {
        let mut c = sys::bison_attributes::default();
        let rc = unsafe { sys::bison_get_method_attributes(self.handle, key(name), &mut c) };
        check(rc, &format!("method_attributes({name:?})"))?;
        Ok(Attributes::from_c(&c))
    }

    // ── Field registration with optional attribute metadata ─────────────

    pub fn add_field_int(
        &mut self,
        name: &str,
        value: i32,
        meta: Option<&Attributes>,
    ) -> Result<(), BisonError> {
        let k = key(name);
        let rc = with_meta_ptr(meta, |m| unsafe {
            sys::bison_add_field_int(self.handle, k, value, m)
        });
        check(rc, &format!("add_field_int[{name}]"))
    }

    pub fn add_field_float(
        &mut self,
        name: &str,
        value: f32,
        meta: Option<&Attributes>,
    ) -> Result<(), BisonError> {
        let k = key(name);
        let rc = with_meta_ptr(meta, |m| unsafe {
            sys::bison_add_field_float(self.handle, k, value, m)
        });
        check(rc, &format!("add_field_float[{name}]"))
    }

    pub fn add_field_bool(
        &mut self,
        name: &str,
        value: bool,
        meta: Option<&Attributes>,
    ) -> Result<(), BisonError> {
        let k = key(name);
        let rc = with_meta_ptr(meta, |m| unsafe {
            sys::bison_add_field_bool(self.handle, k, value as c_int, m)
        });
        check(rc, &format!("add_field_bool[{name}]"))
    }

    pub fn add_field_string(
        &mut self,
        name: &str,
        value: &str,
        meta: Option<&Attributes>,
    ) -> Result<(), BisonError> {
        let k = key(name);
        let c = CString::new(value).map_err(|_| {
            BisonError::new(
                sys::BISON_ERR_EXCEPTION,
                "add_field_string: value contains a NUL byte",
            )
        })?;
        let rc = with_meta_ptr(meta, |m| unsafe {
            sys::bison_add_field_string(self.handle, k, c.as_ptr(), m)
        });
        check(rc, &format!("add_field_string[{name}]"))
    }

    /// Declares a new `bison::key_t`-valued field -- the [`Dynamic::add_field`]
    /// counterpart to [`Dynamic::set_key`], for the same reason `add_field`
    /// itself can't dispatch to this from a plain `i32`.
    pub fn add_field_key(
        &mut self,
        name: &str,
        value: u32,
        meta: Option<&Attributes>,
    ) -> Result<(), BisonError> {
        let k = key(name);
        let rc = with_meta_ptr(meta, |m| unsafe {
            sys::bison_add_field_key(self.handle, k, value, m)
        });
        check(rc, &format!("add_field_key[{name}]"))
    }

    /// Declares a new field with optional documentation metadata, dispatching
    /// on the [`Value`] variant (via `Into<Value>`). Unlike [`Dynamic::set`]
    /// this fails with [`BisonError`] (`BISON_ERR_DUPLICATE`) if the field
    /// already exists. `Value::Object`/`Value::Key` are rejected -- `bison_c.h`
    /// has no `bison_add_field_object`, and a key_t field must go through
    /// [`Dynamic::add_field_key`] for the same reason `set_key` is separate
    /// from `set`.
    pub fn add_field(
        &mut self,
        name: &str,
        value: impl Into<Value>,
        meta: Option<&Attributes>,
    ) -> Result<(), BisonError> {
        match value.into() {
            Value::Bool(v) => self.add_field_bool(name, v, meta),
            Value::Int(v) => self.add_field_int(name, v, meta),
            Value::Float(v) => self.add_field_float(name, v, meta),
            Value::Str(v) => self.add_field_string(name, &v, meta),
            Value::VecBool(v) => self.add_field_vector_bool(name, &v, meta),
            Value::VecInt(v) => self.add_field_vector_int(name, &v, meta),
            Value::VecFloat(v) => self.add_field_vector_float(name, &v, meta),
            Value::Bytes(v) => self.add_field_vector_bytes(name, &v, meta),
            Value::Key(_) | Value::Object(_) => Err(BisonError::new(
                sys::BISON_ERR_TYPE,
                &format!("add_field[{name}]: unsupported value type (use add_field_key, or set() after creation, for objects)"),
            )),
        }
    }

    // ── Serialization ────────────────────────────────────────────────────

    /// Serializes to a JSON string. Pass `indent = -1` for compact output.
    pub fn to_json(&self, indent: i32) -> Result<String, BisonError> {
        let mut out: *mut c_char = ptr::null_mut();
        let rc = unsafe { sys::bison_to_json(self.handle, indent, &mut out) };
        check(rc, "to_json")?;
        let s = unsafe { CStr::from_ptr(out).to_string_lossy().into_owned() };
        unsafe { sys::bison_free_string(out) };
        Ok(s)
    }

    /// Serializes to a YAML string.
    pub fn to_yaml(&self) -> Result<String, BisonError> {
        let mut out: *mut c_char = ptr::null_mut();
        let rc = unsafe { sys::bison_to_yaml(self.handle, &mut out) };
        check(rc, "to_yaml")?;
        let s = unsafe { CStr::from_ptr(out).to_string_lossy().into_owned() };
        unsafe { sys::bison_free_string(out) };
        Ok(s)
    }

    /// Human-readable representation (`bison_print`).
    pub fn pretty(&self, multiline: bool, indent: &str) -> Result<String, BisonError> {
        let indent_c = CString::new(indent).unwrap_or_else(|_| CString::new("  ").unwrap());
        let opts = sys::bison_print_options {
            multiline: multiline as c_int,
            indent: indent_c.as_ptr(),
        };
        let mut out: *mut c_char = ptr::null_mut();
        let rc = unsafe { sys::bison_print(self.handle, &opts, &mut out) };
        check(rc, "pretty")?;
        let s = unsafe { CStr::from_ptr(out).to_string_lossy().into_owned() };
        unsafe { sys::bison_free_string(out) };
        Ok(s)
    }

    /// Serializes to the compact binary wire format (see `FORMAT.md`) -- the
    /// counterpart to module-level [`deserialize`]. Field keys are encoded
    /// as their raw hash, so (unlike [`Dynamic::to_json`]/[`Dynamic::to_yaml`])
    /// this format is self-contained and needs no key-name map to round-trip.
    pub fn serialize(&self) -> Result<Vec<u8>, BisonError> {
        let mut out: *mut u8 = ptr::null_mut();
        let mut out_len: usize = 0;
        let rc = unsafe { sys::bison_serialize(self.handle, &mut out, &mut out_len) };
        check(rc, "serialize")?;
        let v = unsafe { std::slice::from_raw_parts(out, out_len).to_vec() };
        unsafe { sys::bison_free_buffer(out) };
        Ok(v)
    }
}

impl Default for Dynamic {
    fn default() -> Self {
        Dynamic::new("")
    }
}

impl Clone for Dynamic {
    /// Performs a deep clone (`bison_clone`): nested `Dynamic` fields are
    /// recursively cloned too, so the result shares no mutable state with
    /// `self`. `Clone::clone()` is mapped to `bison_clone` rather than
    /// `bison_add_ref` because Rust's `Clone` convention implies an
    /// independent copy; use [`Dynamic::add_ref`] for a new handle sharing
    /// the same underlying object.
    fn clone(&self) -> Self {
        let h = unsafe { sys::bison_clone(self.handle) };
        assert!(!h.is_null(), "bison_clone failed");
        Dynamic::from_raw(h, true)
    }
}

impl Drop for Dynamic {
    fn drop(&mut self) {
        if self.owned && !self.released {
            self.released = true;
            unsafe { sys::bison_release(self.handle) };
        }
        for raw in self.callbacks.drain(..) {
            unsafe { drop(Box::from_raw(raw as *mut Box<MethodCallback>)) };
        }
    }
}

impl fmt::Debug for Dynamic {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Dynamic")
            .field("handle", &self.handle)
            .field("owned", &self.owned)
            .field("size", &self.size())
            .finish()
    }
}

impl fmt::Display for Dynamic {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.pretty(true, "  ").unwrap_or_default())
    }
}

/// Iterator over a [`Dynamic`]'s array-like (numeric-key) elements. Built by
/// [`Dynamic::iter`] / `&Dynamic`'s [`IntoIterator`] impl.
pub struct DynamicIter<'a> {
    dynamic: &'a Dynamic,
    index: usize,
    len: usize,
}

impl Iterator for DynamicIter<'_> {
    type Item = Result<Value, BisonError>;

    fn next(&mut self) -> Option<Self::Item> {
        if self.index >= self.len {
            return None;
        }
        let v = self.dynamic.get_at(self.index);
        self.index += 1;
        Some(v)
    }
}

impl<'a> IntoIterator for &'a Dynamic {
    type Item = Result<Value, BisonError>;
    type IntoIter = DynamicIter<'a>;

    fn into_iter(self) -> Self::IntoIter {
        self.iter()
    }
}

/// Trampoline invoked by the C library for every `bison_add_method`
/// registration in this crate; `user` points at the boxed Rust closure to
/// dispatch to. Panics are caught at this boundary since unwinding across
/// an `extern "C"` call (back into C++) is undefined behavior.
unsafe extern "C" fn method_trampoline(
    self_h: sys::bison_handle,
    params_h: sys::bison_handle,
    result_h: sys::bison_handle,
    user: *mut c_void,
) {
    let closure = &mut *(user as *mut Box<MethodCallback>);
    let self_dyn = Dynamic::from_raw(self_h, false);
    let params_dyn = Dynamic::from_raw(params_h, false);
    let mut result_dyn = Dynamic::from_raw(result_h, false);
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        closure(&self_dyn, &params_dyn, &mut result_dyn);
    }));
}

// ─── Free functions / class registry ────────────────────────────────────────

fn registered_prototypes() -> &'static Mutex<Vec<Dynamic>> {
    static REGISTRY: OnceLock<Mutex<Vec<Dynamic>>> = OnceLock::new();
    REGISTRY.get_or_init(|| Mutex::new(Vec::new()))
}

/// Parses a JSON string and returns the root object.
pub fn from_json(text: &str) -> Result<Dynamic, BisonError> {
    let c = CString::new(text).map_err(|_| {
        BisonError::new(sys::BISON_ERR_PARSE, "from_json: input contains a NUL byte")
    })?;
    let h = unsafe { sys::bison_from_json(c.as_ptr()) };
    if h.is_null() {
        return Err(BisonError::new(
            sys::BISON_ERR_PARSE,
            "from_json: invalid or unsupported JSON",
        ));
    }
    Ok(Dynamic::from_raw(h, true))
}

/// Parses a YAML string and returns the root object.
pub fn from_yaml(text: &str) -> Result<Dynamic, BisonError> {
    let c = CString::new(text).map_err(|_| {
        BisonError::new(sys::BISON_ERR_PARSE, "from_yaml: input contains a NUL byte")
    })?;
    let h = unsafe { sys::bison_from_yaml(c.as_ptr()) };
    if h.is_null() {
        return Err(BisonError::new(
            sys::BISON_ERR_PARSE,
            "from_yaml: invalid or unsupported YAML",
        ));
    }
    Ok(Dynamic::from_raw(h, true))
}

/// Deserializes a buffer produced by [`Dynamic::serialize`].
pub fn deserialize(data: &[u8]) -> Result<Dynamic, BisonError> {
    let mut h: sys::bison_handle = ptr::null_mut();
    let p = if data.is_empty() {
        ptr::null()
    } else {
        data.as_ptr()
    };
    let rc = unsafe { sys::bison_deserialize(p, data.len(), &mut h) };
    check(rc, "deserialize")?;
    Ok(Dynamic::from_raw(h, true))
}

/// Registers `prototype` (whose `__class` field is already set, e.g. via
/// [`Dynamic::new`]) as a class in the global (or named) namespace registry.
///
/// `prototype` is moved into a process-wide keep-alive list so any methods
/// registered on it keep working for as long as the class stays registered
/// -- see [`Dynamic::add_method`]'s doc comment.
pub fn add_class(
    prototype: Dynamic,
    parent_name: &str,
    ns_name: &str,
    meta: Option<&Attributes>,
) -> Result<(), BisonError> {
    let ns_key = key_or_zero(ns_name);
    let parent_key = key_or_zero(parent_name);
    let rc = with_meta_ptr(meta, |m| unsafe {
        sys::bison_add_class(ns_key, prototype.handle, parent_key, m)
    });
    check(rc, &format!("add_class({parent_name:?})"))?;
    registered_prototypes().lock().unwrap().push(prototype);
    Ok(())
}

/// Looks up a registered class prototype. Returns a non-owning view, or
/// `None` if not found.
pub fn find_class(klass_name: &str, ns_name: &str) -> Option<Dynamic> {
    let ns_key = key_or_zero(ns_name);
    let h = unsafe { sys::bison_find_class(ns_key, key(klass_name)) };
    if h.is_null() {
        None
    } else {
        Some(Dynamic::from_raw(h, false))
    }
}

/// Creates a new instance of a registered class (with inherited fields and
/// methods).
pub fn instantiate(klass_name: &str, ns_name: &str) -> Result<Dynamic, BisonError> {
    let ns_key = key_or_zero(ns_name);
    let h = unsafe { sys::bison_instantiate(ns_key, key(klass_name)) };
    if h.is_null() {
        return Err(BisonError::new(
            sys::BISON_ERR_NOT_FOUND,
            &format!("instantiate({klass_name:?})"),
        ));
    }
    Ok(Dynamic::from_raw(h, true))
}

/// Removes all registered classes from the global registry.
pub fn clear_registry() {
    unsafe { sys::bison_clear_registry() };
    registered_prototypes().lock().unwrap().clear();
}

/// Reads the class-level attribute metadata of a registered class.
pub fn class_attributes(klass_name: &str, ns_name: &str) -> Result<Attributes, BisonError> {
    let ns_key = key_or_zero(ns_name);
    let mut c = sys::bison_attributes::default();
    let rc = unsafe { sys::bison_get_class_attributes(ns_key, key(klass_name), &mut c) };
    check(rc, &format!("class_attributes({klass_name:?})"))?;
    Ok(Attributes::from_c(&c))
}

// MIT License © 2025 Binary Dice Games

// Package bison: see native.go for the package-level doc comment and cgo
// preamble. This file is the safe, idiomatic Go wrapper around bison_c.h --
// the Go analogue of bison/dynamic.py's Dynamic class, C#'s Dynamic.cs, and
// Rust's dynamic.rs.
//
// [Dynamic] wraps an opaque bison_handle and exposes it through typed
// getters/setters (GetInt/SetInt, ...) as well as an ergonomic,
// interface{}-based Get/Set pair that cascades through the underlying field
// types the same way the other bindings' generic accessors do. Reference
// counting is handled by Close (idempotent, safe to call multiple times) and
// a runtime.SetFinalizer safety net; callers should still call Close (or
// defer it) explicitly rather than relying on the finalizer, whose timing is
// not guaranteed.
package bison

/*
#include <stdlib.h>
#include "bison_c.h"
#include "shim.h"

// Forward declaration for the //export'd trampoline defined in native.go
// (see shim.c/shim.h for bison_add_method_shim itself).
extern void goMethodTrampoline(bison_handle self, bison_handle params, bison_handle result, void* user);
*/
import "C"

import (
	"fmt"
	"io"
	"math"
	"runtime"
	"runtime/cgo"
	"sync"
	"unsafe"
)

// ─── Name hashing ───────────────────────────────────────────────────────────

// keyCacheMax bounds the size of the Key() memoization cache: real callers
// draw field/method names from a small, static, schema-defined set reused
// across many calls, so caching turns most lookups into a map hit instead of
// a string copy + cgo call; the bound keeps a caller hashing high-cardinality
// or externally-derived strings from growing the cache without bound (the
// same tradeoff bison.dynamic.key()'s functools.lru_cache(maxsize=4096) and
// Rust's KEY_CACHE_MAX make).
const keyCacheMax = 4096

var (
	keyCacheMu sync.Mutex
	keyCache   = make(map[string]uint32)
)

// Key returns the 32-bit FNV-1a hash of name (identical to "name"_key in C++
// and bison.key(name) in Python). Go, like Python/C#/Rust, has no way to
// hash a string literal at compile time, so every Dynamic field/method
// access funnels through this function at run time; results are memoized
// (bounded at keyCacheMax entries).
func Key(name string) uint32 {
	keyCacheMu.Lock()
	if h, ok := keyCache[name]; ok {
		keyCacheMu.Unlock()
		return h
	}
	keyCacheMu.Unlock()

	cName := C.CString(name)
	defer C.free(unsafe.Pointer(cName))
	hash := uint32(C.bison_key(cName))

	keyCacheMu.Lock()
	if len(keyCache) < keyCacheMax {
		keyCache[name] = hash
	}
	keyCacheMu.Unlock()
	return hash
}

func cKey(name string) C.bison_hash {
	return C.bison_hash(Key(name))
}

func cKeyOrZero(name string) C.bison_hash {
	if name == "" {
		return 0
	}
	return cKey(name)
}

// resolveKeyValue implements the interface{} contract shared by SetKey,
// SetKeyAt, and AddFieldKey: value is either an already-hashed uint32 (e.g.
// from Key()) or a string name, hashed the same way Key() would.
func resolveKeyValue(value interface{}) (uint32, error) {
	switch v := value.(type) {
	case uint32:
		return v, nil
	case string:
		return Key(v), nil
	default:
		return 0, fmt.Errorf("bison: key value must be uint32 or string, got %T", value)
	}
}

// ─── Errors ─────────────────────────────────────────────────────────────────

// bison_error codes (see bison_c.h), exported as typed constants so callers
// can compare a *BisonError's Code field directly.
const (
	ErrNull      int32 = -1 // A required handle or pointer argument was NULL.
	ErrType      int32 = -2 // The field holds a different type than requested.
	ErrNotFound  int32 = -3 // Method or field not found.
	ErrDuplicate int32 = -4 // Attempted to add a duplicate class or method.
	ErrException int32 = -5 // An unexpected C++ exception was caught.
	ErrParse     int32 = -6 // Input failed to parse (JSON / YAML / bison_deserialize()).
)

var bisonErrorMessages = map[int32]string{
	ErrNull:      "Null handle or pointer",
	ErrType:      "Field type mismatch",
	ErrNotFound:  "Method or field not found",
	ErrDuplicate: "Duplicate class or method",
	ErrException: "Internal C++ exception",
	ErrParse:     "Parse error (JSON / YAML / binary buffer)",
}

func bisonErrorMessage(code int32) string {
	if msg, ok := bisonErrorMessages[code]; ok {
		return msg
	}
	return fmt.Sprintf("Unknown error %d", code)
}

// BisonError is returned when a bison_* C API call reports a non-zero error
// code. Code is the raw bison_error value (see bison_c.h); compare it
// against the Err* constants, or use errors.As to recover a *BisonError from
// a wrapped error.
type BisonError struct {
	Code    int32
	message string
}

func (e *BisonError) Error() string { return e.message }

func newBisonError(code int32, context string) *BisonError {
	msg := bisonErrorMessage(code)
	if context != "" {
		msg = context + ": " + msg
	}
	return &BisonError{Code: code, message: msg}
}

func checkBison(rc C.bison_error, context string) error {
	if rc == C.BISON_OK {
		return nil
	}
	return newBisonError(int32(rc), context)
}

// ─── Attributes ─────────────────────────────────────────────────────────────

// Attributes carries optional display/documentation metadata for a class,
// field, or method (mirrors bison_attributes). An empty string means the
// corresponding attribute is unset (encoded as a NULL pointer at the C ABI
// boundary), matching the other bindings' Optional[str]/Option<String>.
type Attributes struct {
	DisplayName     string
	Description     string
	Category        string
	Obsolete        bool
	ObsoleteMessage string
	Required        bool
}

// attrsToC builds a *C.bison_attributes from meta (nil for a nil meta),
// returning a cleanup function that must be called (typically via defer)
// once the pointer is no longer needed by any in-flight C call.
func attrsToC(meta *Attributes) (*C.bison_attributes, func()) {
	if meta == nil {
		return nil, func() {}
	}
	var c C.bison_attributes
	var owned []*C.char

	set := func(dst **C.char, s string) {
		if s == "" {
			*dst = nil
			return
		}
		cs := C.CString(s)
		owned = append(owned, cs)
		*dst = cs
	}
	set(&c.display_name, meta.DisplayName)
	set(&c.description, meta.Description)
	set(&c.category, meta.Category)
	set(&c.obsolete_message, meta.ObsoleteMessage)
	if meta.Obsolete {
		c.obsolete = 1
	}
	if meta.Required {
		c.required = 1
	}

	free := func() {
		for _, cs := range owned {
			C.free(unsafe.Pointer(cs))
		}
	}
	return &c, free
}

func attrsFromC(c C.bison_attributes) Attributes {
	dec := func(p *C.char) string {
		if p == nil {
			return ""
		}
		return C.GoString(p)
	}
	return Attributes{
		DisplayName:     dec(c.display_name),
		Description:     dec(c.description),
		Category:        dec(c.category),
		Obsolete:        c.obsolete != 0,
		ObsoleteMessage: dec(c.obsolete_message),
		Required:        c.required != 0,
	}
}

// ─── Dynamic ────────────────────────────────────────────────────────────────

// Dynamic is a reference-counted Bison dynamic object. It wraps a
// bison_handle; owned == true (the common case) means this value holds a
// reference that Close releases, while owned == false marks a non-owning
// view (self/params/result inside a method callback, or a FindClass lookup
// result) whose Close is a no-op on the handle.
type Dynamic struct {
	handle    C.bison_handle
	owned     bool
	released  bool
	callbacks []cgo.Handle // AddMethod registrations, freed on Close.
}

var _ io.Closer = (*Dynamic)(nil)

func newOwned(h C.bison_handle) *Dynamic {
	d := &Dynamic{handle: h, owned: true}
	runtime.SetFinalizer(d, (*Dynamic).finalize)
	return d
}

func newView(h C.bison_handle) *Dynamic {
	return &Dynamic{handle: h, owned: false}
}

func (d *Dynamic) finalize() {
	_ = d.Close()
}

// New creates a new, empty dynamic object. Pass "" for an anonymous object
// (equivalent to NewAnonymous()).
func New(className string) (*Dynamic, error) {
	h := C.bison_create(cKeyOrZero(className))
	if h == nil {
		return nil, fmt.Errorf("bison: bison_create failed")
	}
	return newOwned(h), nil
}

// NewAnonymous creates a new, empty anonymous dynamic object (equivalent to
// New("")).
func NewAnonymous() (*Dynamic, error) {
	return New("")
}

// ── Lifecycle ───────────────────────────────────────────────────────────

// Close releases the underlying handle, decrementing its reference count.
// Safe to call multiple times (subsequent calls are no-ops) and safe to call
// on a non-owning view (also a no-op on the handle). Prefer calling Close
// explicitly (typically via defer) over relying on the finalizer, whose
// timing is not guaranteed.
func (d *Dynamic) Close() error {
	if d.owned && !d.released {
		d.released = true
		C.bison_release(d.handle)
	}
	for _, h := range d.callbacks {
		h.Delete()
	}
	d.callbacks = nil
	return nil
}

// AddRef returns a new Dynamic sharing ownership of the same underlying
// object (bison_add_ref). Both values must be closed independently.
func (d *Dynamic) AddRef() (*Dynamic, error) {
	h := C.bison_add_ref(d.handle)
	if h == nil {
		return nil, fmt.Errorf("bison: bison_add_ref failed")
	}
	return newOwned(h), nil
}

// Clone performs a deep clone (bison_clone): nested Dynamic fields are
// recursively cloned too, so the result shares no mutable state with d. Use
// AddRef instead for a new handle sharing the same underlying object.
func (d *Dynamic) Clone() (*Dynamic, error) {
	h := C.bison_clone(d.handle)
	if h == nil {
		return nil, fmt.Errorf("bison: bison_clone failed")
	}
	return newOwned(h), nil
}

// ── Scalar field access -- named ────────────────────────────────────────

// GetInt reads an int32 field by name.
func (d *Dynamic) GetInt(name string) (int32, error) {
	var out C.int32_t
	rc := C.bison_get_int(d.handle, cKey(name), &out)
	if err := checkBison(rc, fmt.Sprintf("get_int[%s]", name)); err != nil {
		return 0, err
	}
	return int32(out), nil
}

// SetInt sets an int32 field by name.
func (d *Dynamic) SetInt(name string, value int32) error {
	rc := C.bison_set_int(d.handle, cKey(name), C.int32_t(value))
	return checkBison(rc, fmt.Sprintf("set_int[%s]", name))
}

// GetFloat reads a float32 field by name.
func (d *Dynamic) GetFloat(name string) (float32, error) {
	var out C.float
	rc := C.bison_get_float(d.handle, cKey(name), &out)
	if err := checkBison(rc, fmt.Sprintf("get_float[%s]", name)); err != nil {
		return 0, err
	}
	return float32(out), nil
}

// SetFloat sets a float32 field by name.
func (d *Dynamic) SetFloat(name string, value float32) error {
	rc := C.bison_set_float(d.handle, cKey(name), C.float(value))
	return checkBison(rc, fmt.Sprintf("set_float[%s]", name))
}

// GetBool reads a bool field by name.
func (d *Dynamic) GetBool(name string) (bool, error) {
	var out C.int
	rc := C.bison_get_bool(d.handle, cKey(name), &out)
	if err := checkBison(rc, fmt.Sprintf("get_bool[%s]", name)); err != nil {
		return false, err
	}
	return out != 0, nil
}

// SetBool sets a bool field by name.
func (d *Dynamic) SetBool(name string, value bool) error {
	var cv C.int
	if value {
		cv = 1
	}
	rc := C.bison_set_bool(d.handle, cKey(name), cv)
	return checkBison(rc, fmt.Sprintf("set_bool[%s]", name))
}

// GetString reads a string field by name.
func (d *Dynamic) GetString(name string) (string, error) {
	k := cKey(name)
	var lenOut C.size_t
	rc := C.bison_get_string(d.handle, k, nil, 0, &lenOut)
	if err := checkBison(rc, fmt.Sprintf("get_string[%s]", name)); err != nil {
		return "", err
	}
	if lenOut == 0 {
		return "", nil
	}
	buf := make([]byte, int(lenOut)+1)
	rc2 := C.bison_get_string(d.handle, k, (*C.char)(unsafe.Pointer(&buf[0])), C.size_t(len(buf)), nil)
	if err := checkBison(rc2, fmt.Sprintf("get_string[%s]", name)); err != nil {
		return "", err
	}
	return string(buf[:lenOut]), nil
}

// SetString sets a string field by name.
func (d *Dynamic) SetString(name string, value string) error {
	cValue := C.CString(value)
	defer C.free(unsafe.Pointer(cValue))
	rc := C.bison_set_string(d.handle, cKey(name), cValue)
	return checkBison(rc, fmt.Sprintf("set_string[%s]", name))
}

// GetKey reads a bison::key_t-valued field by name (e.g. an object's "id",
// or an enum-like selector), returning its raw hash. This is a distinct
// field-variant type from int32_t -- see SetKey.
func (d *Dynamic) GetKey(name string) (uint32, error) {
	var out C.bison_hash
	rc := C.bison_get_key(d.handle, cKey(name), &out)
	if err := checkBison(rc, fmt.Sprintf("get_key[%s]", name)); err != nil {
		return 0, err
	}
	return uint32(out), nil
}

// SetKey sets a bison::key_t-valued field by name. value is either an
// already-hashed uint32 (e.g. from Key()) or a string name, hashed the same
// way Key() would -- unlike SetInt, a plain int32 handed to Set is always
// written as int32, since there is no way to tell "this should become a
// key_t field" from context alone.
func (d *Dynamic) SetKey(name string, value interface{}) error {
	v, err := resolveKeyValue(value)
	if err != nil {
		return err
	}
	rc := C.bison_set_key(d.handle, cKey(name), C.bison_hash(v))
	return checkBison(rc, fmt.Sprintf("set_key[%s]", name))
}

// GetObject reads a nested object field by name. A nil *Dynamic with a nil
// error indicates a null object reference (distinct from a not-found/type
// error).
func (d *Dynamic) GetObject(name string) (*Dynamic, error) {
	var out C.bison_handle
	rc := C.bison_get_object(d.handle, cKey(name), &out)
	if err := checkBison(rc, fmt.Sprintf("get_object[%s]", name)); err != nil {
		return nil, err
	}
	if out == nil {
		return nil, nil
	}
	return newOwned(out), nil
}

// SetObject sets a nested object field by name. The library increments
// value's ref-count, so value remains independently owned by the caller;
// pass nil to set a null object reference.
func (d *Dynamic) SetObject(name string, value *Dynamic) error {
	var h C.bison_handle
	if value != nil {
		h = value.handle
	}
	rc := C.bison_set_object(d.handle, cKey(name), h)
	return checkBison(rc, fmt.Sprintf("set_object[%s]", name))
}

// ── Scalar field access -- indexed ──────────────────────────────────────

// GetIntAt reads an int32 field by numeric index.
func (d *Dynamic) GetIntAt(index int) (int32, error) {
	var out C.int32_t
	rc := C.bison_get_int_at(d.handle, C.size_t(index), &out)
	if err := checkBison(rc, fmt.Sprintf("get_int_at[%d]", index)); err != nil {
		return 0, err
	}
	return int32(out), nil
}

// SetIntAt sets an int32 field by numeric index.
func (d *Dynamic) SetIntAt(index int, value int32) error {
	rc := C.bison_set_int_at(d.handle, C.size_t(index), C.int32_t(value))
	return checkBison(rc, fmt.Sprintf("set_int_at[%d]", index))
}

// GetFloatAt reads a float32 field by numeric index.
func (d *Dynamic) GetFloatAt(index int) (float32, error) {
	var out C.float
	rc := C.bison_get_float_at(d.handle, C.size_t(index), &out)
	if err := checkBison(rc, fmt.Sprintf("get_float_at[%d]", index)); err != nil {
		return 0, err
	}
	return float32(out), nil
}

// SetFloatAt sets a float32 field by numeric index.
func (d *Dynamic) SetFloatAt(index int, value float32) error {
	rc := C.bison_set_float_at(d.handle, C.size_t(index), C.float(value))
	return checkBison(rc, fmt.Sprintf("set_float_at[%d]", index))
}

// GetBoolAt reads a bool field by numeric index.
func (d *Dynamic) GetBoolAt(index int) (bool, error) {
	var out C.int
	rc := C.bison_get_bool_at(d.handle, C.size_t(index), &out)
	if err := checkBison(rc, fmt.Sprintf("get_bool_at[%d]", index)); err != nil {
		return false, err
	}
	return out != 0, nil
}

// SetBoolAt sets a bool field by numeric index.
func (d *Dynamic) SetBoolAt(index int, value bool) error {
	var cv C.int
	if value {
		cv = 1
	}
	rc := C.bison_set_bool_at(d.handle, C.size_t(index), cv)
	return checkBison(rc, fmt.Sprintf("set_bool_at[%d]", index))
}

// GetStringAt reads a string field by numeric index.
func (d *Dynamic) GetStringAt(index int) (string, error) {
	var lenOut C.size_t
	rc := C.bison_get_string_at(d.handle, C.size_t(index), nil, 0, &lenOut)
	if err := checkBison(rc, fmt.Sprintf("get_string_at[%d]", index)); err != nil {
		return "", err
	}
	if lenOut == 0 {
		return "", nil
	}
	buf := make([]byte, int(lenOut)+1)
	rc2 := C.bison_get_string_at(d.handle, C.size_t(index), (*C.char)(unsafe.Pointer(&buf[0])), C.size_t(len(buf)), nil)
	if err := checkBison(rc2, fmt.Sprintf("get_string_at[%d]", index)); err != nil {
		return "", err
	}
	return string(buf[:lenOut]), nil
}

// SetStringAt sets a string field by numeric index.
func (d *Dynamic) SetStringAt(index int, value string) error {
	cValue := C.CString(value)
	defer C.free(unsafe.Pointer(cValue))
	rc := C.bison_set_string_at(d.handle, C.size_t(index), cValue)
	return checkBison(rc, fmt.Sprintf("set_string_at[%d]", index))
}

// GetKeyAt is the indexed counterpart to GetKey.
func (d *Dynamic) GetKeyAt(index int) (uint32, error) {
	var out C.bison_hash
	rc := C.bison_get_key_at(d.handle, C.size_t(index), &out)
	if err := checkBison(rc, fmt.Sprintf("get_key_at[%d]", index)); err != nil {
		return 0, err
	}
	return uint32(out), nil
}

// SetKeyAt is the indexed counterpart to SetKey, for the same reason a bare
// SetAt(index, value) int assignment can't dispatch to it.
func (d *Dynamic) SetKeyAt(index int, value interface{}) error {
	v, err := resolveKeyValue(value)
	if err != nil {
		return err
	}
	rc := C.bison_set_key_at(d.handle, C.size_t(index), C.bison_hash(v))
	return checkBison(rc, fmt.Sprintf("set_key_at[%d]", index))
}

// GetObjectAt reads a nested object field by numeric index.
func (d *Dynamic) GetObjectAt(index int) (*Dynamic, error) {
	var out C.bison_handle
	rc := C.bison_get_object_at(d.handle, C.size_t(index), &out)
	if err := checkBison(rc, fmt.Sprintf("get_object_at[%d]", index)); err != nil {
		return nil, err
	}
	if out == nil {
		return nil, nil
	}
	return newOwned(out), nil
}

// SetObjectAt sets a nested object field by numeric index; pass nil to set a
// null object reference.
func (d *Dynamic) SetObjectAt(index int, value *Dynamic) error {
	var h C.bison_handle
	if value != nil {
		h = value.handle
	}
	rc := C.bison_set_object_at(d.handle, C.size_t(index), h)
	return checkBison(rc, fmt.Sprintf("set_object_at[%d]", index))
}

// ── Vector field access ─────────────────────────────────────────────────

// GetVectorBool reads a vector<bool> field.
func (d *Dynamic) GetVectorBool(name string) ([]bool, error) {
	k := cKey(name)
	var lenOut C.size_t
	rc := C.bison_get_vector_bool(d.handle, k, nil, 0, &lenOut)
	if err := checkBison(rc, fmt.Sprintf("get_vector_bool[%s]", name)); err != nil {
		return nil, err
	}
	if lenOut == 0 {
		return []bool{}, nil
	}
	buf := make([]C.int, int(lenOut))
	rc2 := C.bison_get_vector_bool(d.handle, k, &buf[0], lenOut, nil)
	if err := checkBison(rc2, fmt.Sprintf("get_vector_bool[%s]", name)); err != nil {
		return nil, err
	}
	out := make([]bool, len(buf))
	for i, v := range buf {
		out[i] = v != 0
	}
	return out, nil
}

// SetVectorBool replaces the contents of a vector<bool> field, auto-vivifying
// it if absent.
func (d *Dynamic) SetVectorBool(name string, values []bool) error {
	k := cKey(name)
	ints := make([]C.int, len(values))
	for i, v := range values {
		if v {
			ints[i] = 1
		}
	}
	var ptr *C.int
	if len(ints) > 0 {
		ptr = &ints[0]
	}
	rc := C.bison_set_vector_bool(d.handle, k, ptr, C.size_t(len(ints)))
	return checkBison(rc, fmt.Sprintf("set_vector_bool[%s]", name))
}

// AddFieldVectorBool declares a new vector<bool> field with optional
// documentation metadata. Fails with BisonError (ErrDuplicate) if the field
// already exists.
func (d *Dynamic) AddFieldVectorBool(name string, values []bool, meta *Attributes) error {
	k := cKey(name)
	ints := make([]C.int, len(values))
	for i, v := range values {
		if v {
			ints[i] = 1
		}
	}
	var ptr *C.int
	if len(ints) > 0 {
		ptr = &ints[0]
	}
	cMeta, freeMeta := attrsToC(meta)
	defer freeMeta()
	rc := C.bison_add_field_vector_bool(d.handle, k, ptr, C.size_t(len(ints)), cMeta)
	return checkBison(rc, fmt.Sprintf("add_field_vector_bool[%s]", name))
}

// GetVectorInt reads a vector<int32_t> field.
func (d *Dynamic) GetVectorInt(name string) ([]int32, error) {
	k := cKey(name)
	var lenOut C.size_t
	rc := C.bison_get_vector_int(d.handle, k, nil, 0, &lenOut)
	if err := checkBison(rc, fmt.Sprintf("get_vector_int[%s]", name)); err != nil {
		return nil, err
	}
	if lenOut == 0 {
		return []int32{}, nil
	}
	buf := make([]int32, int(lenOut))
	rc2 := C.bison_get_vector_int(d.handle, k, (*C.int32_t)(unsafe.Pointer(&buf[0])), lenOut, nil)
	if err := checkBison(rc2, fmt.Sprintf("get_vector_int[%s]", name)); err != nil {
		return nil, err
	}
	return buf, nil
}

// SetVectorInt replaces the contents of a vector<int32_t> field,
// auto-vivifying it if absent.
func (d *Dynamic) SetVectorInt(name string, values []int32) error {
	k := cKey(name)
	var ptr *C.int32_t
	if len(values) > 0 {
		ptr = (*C.int32_t)(unsafe.Pointer(&values[0]))
	}
	rc := C.bison_set_vector_int(d.handle, k, ptr, C.size_t(len(values)))
	return checkBison(rc, fmt.Sprintf("set_vector_int[%s]", name))
}

// AddFieldVectorInt declares a new vector<int32_t> field with optional
// documentation metadata. Fails with BisonError (ErrDuplicate) if the field
// already exists.
func (d *Dynamic) AddFieldVectorInt(name string, values []int32, meta *Attributes) error {
	k := cKey(name)
	var ptr *C.int32_t
	if len(values) > 0 {
		ptr = (*C.int32_t)(unsafe.Pointer(&values[0]))
	}
	cMeta, freeMeta := attrsToC(meta)
	defer freeMeta()
	rc := C.bison_add_field_vector_int(d.handle, k, ptr, C.size_t(len(values)), cMeta)
	return checkBison(rc, fmt.Sprintf("add_field_vector_int[%s]", name))
}

// GetVectorFloat reads a vector<float> field.
func (d *Dynamic) GetVectorFloat(name string) ([]float32, error) {
	k := cKey(name)
	var lenOut C.size_t
	rc := C.bison_get_vector_float(d.handle, k, nil, 0, &lenOut)
	if err := checkBison(rc, fmt.Sprintf("get_vector_float[%s]", name)); err != nil {
		return nil, err
	}
	if lenOut == 0 {
		return []float32{}, nil
	}
	buf := make([]float32, int(lenOut))
	rc2 := C.bison_get_vector_float(d.handle, k, (*C.float)(unsafe.Pointer(&buf[0])), lenOut, nil)
	if err := checkBison(rc2, fmt.Sprintf("get_vector_float[%s]", name)); err != nil {
		return nil, err
	}
	return buf, nil
}

// SetVectorFloat replaces the contents of a vector<float> field,
// auto-vivifying it if absent.
func (d *Dynamic) SetVectorFloat(name string, values []float32) error {
	k := cKey(name)
	var ptr *C.float
	if len(values) > 0 {
		ptr = (*C.float)(unsafe.Pointer(&values[0]))
	}
	rc := C.bison_set_vector_float(d.handle, k, ptr, C.size_t(len(values)))
	return checkBison(rc, fmt.Sprintf("set_vector_float[%s]", name))
}

// AddFieldVectorFloat declares a new vector<float> field with optional
// documentation metadata. Fails with BisonError (ErrDuplicate) if the field
// already exists.
func (d *Dynamic) AddFieldVectorFloat(name string, values []float32, meta *Attributes) error {
	k := cKey(name)
	var ptr *C.float
	if len(values) > 0 {
		ptr = (*C.float)(unsafe.Pointer(&values[0]))
	}
	cMeta, freeMeta := attrsToC(meta)
	defer freeMeta()
	rc := C.bison_add_field_vector_float(d.handle, k, ptr, C.size_t(len(values)), cMeta)
	return checkBison(rc, fmt.Sprintf("add_field_vector_float[%s]", name))
}

// GetVectorBytes reads a vector<uint8_t> (raw byte buffer) field.
func (d *Dynamic) GetVectorBytes(name string) ([]byte, error) {
	k := cKey(name)
	var lenOut C.size_t
	rc := C.bison_get_vector_bytes(d.handle, k, nil, 0, &lenOut)
	if err := checkBison(rc, fmt.Sprintf("get_vector_bytes[%s]", name)); err != nil {
		return nil, err
	}
	if lenOut == 0 {
		return []byte{}, nil
	}
	buf := make([]byte, int(lenOut))
	rc2 := C.bison_get_vector_bytes(d.handle, k, (*C.uint8_t)(unsafe.Pointer(&buf[0])), lenOut, nil)
	if err := checkBison(rc2, fmt.Sprintf("get_vector_bytes[%s]", name)); err != nil {
		return nil, err
	}
	return buf, nil
}

// SetVectorBytes replaces the contents of a vector<uint8_t> field,
// auto-vivifying it if absent.
func (d *Dynamic) SetVectorBytes(name string, values []byte) error {
	k := cKey(name)
	var ptr *C.uint8_t
	if len(values) > 0 {
		ptr = (*C.uint8_t)(unsafe.Pointer(&values[0]))
	}
	rc := C.bison_set_vector_bytes(d.handle, k, ptr, C.size_t(len(values)))
	return checkBison(rc, fmt.Sprintf("set_vector_bytes[%s]", name))
}

// AddFieldVectorBytes declares a new vector<uint8_t> field with optional
// documentation metadata. Fails with BisonError (ErrDuplicate) if the field
// already exists.
func (d *Dynamic) AddFieldVectorBytes(name string, values []byte, meta *Attributes) error {
	k := cKey(name)
	var ptr *C.uint8_t
	if len(values) > 0 {
		ptr = (*C.uint8_t)(unsafe.Pointer(&values[0]))
	}
	cMeta, freeMeta := attrsToC(meta)
	defer freeMeta()
	rc := C.bison_add_field_vector_bytes(d.handle, k, ptr, C.size_t(len(values)), cMeta)
	return checkBison(rc, fmt.Sprintf("add_field_vector_bytes[%s]", name))
}

// ── Ergonomic generic access ────────────────────────────────────────────

// clampInt converts a plain Go int to int32, erroring if it does not fit --
// the ABI's field is a fixed-width int32_t. A plain int literal (e.g.
// d.Set("hp", 100)) has Go's default int type, not int32, so Set/SetAt/
// AddField accept it too via this conversion for ergonomics.
func clampInt(v int) (int32, error) {
	if v < math.MinInt32 || v > math.MaxInt32 {
		return 0, fmt.Errorf("bison: int value %d does not fit in int32", v)
	}
	return int32(v), nil
}

// Get reads a field by name, probing each scalar/vector getter in turn (int
// -> float -> bool -> string -> object -> key -> vectors), the same cascade
// order the other bindings' generic getters use. The returned value's
// concrete type is one of: int32, float32, bool, string, *Dynamic, uint32
// (a key_t field), []bool, []int32, []float32, or []byte. An untouched
// field auto-vivifies as int32 zero (dynamic::operator[]'s own semantics),
// so this only reaches a later branch once the field is already known to
// hold that type.
func (d *Dynamic) Get(name string) (interface{}, error) {
	if v, err := d.GetInt(name); err == nil {
		return v, nil
	}
	if v, err := d.GetFloat(name); err == nil {
		return v, nil
	}
	if v, err := d.GetBool(name); err == nil {
		return v, nil
	}
	if v, err := d.GetString(name); err == nil {
		return v, nil
	}
	if v, err := d.GetObject(name); err == nil {
		return v, nil
	}
	if v, err := d.GetKey(name); err == nil {
		return v, nil
	}
	if v, err := d.GetVectorInt(name); err == nil {
		return v, nil
	}
	if v, err := d.GetVectorFloat(name); err == nil {
		return v, nil
	}
	if v, err := d.GetVectorBool(name); err == nil {
		return v, nil
	}
	if v, err := d.GetVectorBytes(name); err == nil {
		return v, nil
	}
	return nil, newBisonError(ErrNotFound, fmt.Sprintf("get[%s]: field not found or has an unsupported type", name))
}

// Set writes a field by name, type-switching on value's concrete type:
// bool, int32 (a plain int is also accepted and range-checked into int32),
// float32, string, *Dynamic, nil (a null object reference), []byte, []bool,
// []int32, or []float32.
func (d *Dynamic) Set(name string, value interface{}) error {
	switch v := value.(type) {
	case bool:
		return d.SetBool(name, v)
	case int32:
		return d.SetInt(name, v)
	case int:
		i32, err := clampInt(v)
		if err != nil {
			return err
		}
		return d.SetInt(name, i32)
	case float32:
		return d.SetFloat(name, v)
	case string:
		return d.SetString(name, v)
	case *Dynamic:
		return d.SetObject(name, v)
	case nil:
		return d.SetObject(name, nil)
	case []byte:
		return d.SetVectorBytes(name, v)
	case []bool:
		return d.SetVectorBool(name, v)
	case []int32:
		return d.SetVectorInt(name, v)
	case []float32:
		return d.SetVectorFloat(name, v)
	default:
		return fmt.Errorf("bison: unsupported value type for Set: %T", value)
	}
}

// GetAt is the indexed counterpart to Get (int -> float -> bool -> string ->
// object -> key; bison_c.h has no indexed vector getters).
func (d *Dynamic) GetAt(index int) (interface{}, error) {
	if v, err := d.GetIntAt(index); err == nil {
		return v, nil
	}
	if v, err := d.GetFloatAt(index); err == nil {
		return v, nil
	}
	if v, err := d.GetBoolAt(index); err == nil {
		return v, nil
	}
	if v, err := d.GetStringAt(index); err == nil {
		return v, nil
	}
	if v, err := d.GetObjectAt(index); err == nil {
		return v, nil
	}
	if v, err := d.GetKeyAt(index); err == nil {
		return v, nil
	}
	return nil, newBisonError(ErrNotFound, fmt.Sprintf("get_at[%d]: index not found or has an unsupported type", index))
}

// SetAt is the indexed counterpart to Set. Vector-typed values have no
// indexed form (bison_c.h exports no bison_set_vector_*_at) -- only named
// fields support them.
func (d *Dynamic) SetAt(index int, value interface{}) error {
	switch v := value.(type) {
	case bool:
		return d.SetBoolAt(index, v)
	case int32:
		return d.SetIntAt(index, v)
	case int:
		i32, err := clampInt(v)
		if err != nil {
			return err
		}
		return d.SetIntAt(index, i32)
	case float32:
		return d.SetFloatAt(index, v)
	case string:
		return d.SetStringAt(index, v)
	case *Dynamic:
		return d.SetObjectAt(index, v)
	case nil:
		return d.SetObjectAt(index, nil)
	case []byte, []bool, []int32, []float32:
		return newBisonError(ErrType, fmt.Sprintf("set_at[%d]: vector-typed values have no indexed form", index))
	default:
		return fmt.Errorf("bison: unsupported value type for SetAt: %T", value)
	}
}

// Each calls fn once per array-like (numeric-key) element, in index order,
// stopping (and returning fn's error) at the first error either GetAt or fn
// itself returns.
func (d *Dynamic) Each(fn func(index int, value interface{}) error) error {
	n := d.Size()
	for i := 0; i < n; i++ {
		v, err := d.GetAt(i)
		if err != nil {
			return err
		}
		if err := fn(i, v); err != nil {
			return err
		}
	}
	return nil
}

// ── Array-like helpers ───────────────────────────────────────────────────

// Size returns the number of array-like (numeric-key) elements.
func (d *Dynamic) Size() int {
	return int(C.bison_size(d.handle))
}

// ── Methods ──────────────────────────────────────────────────────────────

// AddMethod registers a Go closure as a named method on this object. fn is
// invoked with non-owning views of self/params/result and must populate
// result in place (it must not be closed by fn). fn may be called from any
// goroutine/thread that dispatches a call to this object (an RMI server
// worker, in particular) -- not necessarily the one that called AddMethod --
// and any panic inside fn is recovered and swallowed at the C ABI boundary
// (it must never unwind into C++).
//
// bison_add_class() copies field/method data out of a prototype into the
// C++ registry -- it does not take ownership of the handle -- but a
// registered method only copies the raw C function pointer, i.e. the
// trampoline behind this closure. That trampoline is only kept alive by
// this Dynamic's own callbacks slice, freed on Close; objects later
// instantiated from the class (Instantiate) call back into it directly. So
// a prototype registered via AddClass must be kept alive (not Closed) for
// as long as its class stays in the registry -- AddClass does this itself
// by keeping a package-level reference, mirroring
// bison.dynamic._registered_prototypes / Rust's registered_prototypes().
func (d *Dynamic) AddMethod(name string, fn func(self, params, result *Dynamic)) error {
	h := cgo.NewHandle(fn)
	rc := C.bison_add_method_shim(d.handle, cKey(name), C.bison_method_fn(C.goMethodTrampoline), C.uintptr_t(h), nil)
	if rc != C.BISON_OK {
		h.Delete()
		return checkBison(rc, fmt.Sprintf("add_method(%q)", name))
	}
	d.callbacks = append(d.callbacks, h)
	return nil
}

// Call invokes a named method on this object and returns its result.
func (d *Dynamic) Call(name string, params *Dynamic) (*Dynamic, error) {
	if params == nil {
		scratch, err := New("")
		if err != nil {
			return nil, err
		}
		defer scratch.Close()
		params = scratch
	}
	var result C.bison_handle
	rc := C.bison_call(d.handle, cKey(name), params.handle, &result)
	if err := checkBison(rc, fmt.Sprintf("call(%q)", name)); err != nil {
		return nil, err
	}
	return newOwned(result), nil
}

// ── Field / method attributes ────────────────────────────────────────────

// FieldAttributes reads the attribute metadata for a named field.
func (d *Dynamic) FieldAttributes(name string) (Attributes, error) {
	var c C.bison_attributes
	rc := C.bison_get_field_attributes(d.handle, cKey(name), &c)
	if err := checkBison(rc, fmt.Sprintf("field_attributes(%q)", name)); err != nil {
		return Attributes{}, err
	}
	return attrsFromC(c), nil
}

// MethodAttributes reads the attribute metadata for a named method.
func (d *Dynamic) MethodAttributes(name string) (Attributes, error) {
	var c C.bison_attributes
	rc := C.bison_get_method_attributes(d.handle, cKey(name), &c)
	if err := checkBison(rc, fmt.Sprintf("method_attributes(%q)", name)); err != nil {
		return Attributes{}, err
	}
	return attrsFromC(c), nil
}

// ── Field registration with optional attribute metadata ─────────────────

// AddFieldInt declares a new int32 field with optional documentation
// metadata. Unlike SetInt this fails with BisonError (ErrDuplicate) if the
// field already exists.
func (d *Dynamic) AddFieldInt(name string, value int32, meta *Attributes) error {
	cMeta, freeMeta := attrsToC(meta)
	defer freeMeta()
	rc := C.bison_add_field_int(d.handle, cKey(name), C.int32_t(value), cMeta)
	return checkBison(rc, fmt.Sprintf("add_field_int[%s]", name))
}

// AddFieldFloat declares a new float32 field with optional documentation
// metadata. Fails with BisonError (ErrDuplicate) if the field already
// exists.
func (d *Dynamic) AddFieldFloat(name string, value float32, meta *Attributes) error {
	cMeta, freeMeta := attrsToC(meta)
	defer freeMeta()
	rc := C.bison_add_field_float(d.handle, cKey(name), C.float(value), cMeta)
	return checkBison(rc, fmt.Sprintf("add_field_float[%s]", name))
}

// AddFieldBool declares a new bool field with optional documentation
// metadata. Fails with BisonError (ErrDuplicate) if the field already
// exists.
func (d *Dynamic) AddFieldBool(name string, value bool, meta *Attributes) error {
	var cv C.int
	if value {
		cv = 1
	}
	cMeta, freeMeta := attrsToC(meta)
	defer freeMeta()
	rc := C.bison_add_field_bool(d.handle, cKey(name), cv, cMeta)
	return checkBison(rc, fmt.Sprintf("add_field_bool[%s]", name))
}

// AddFieldString declares a new string field with optional documentation
// metadata. Fails with BisonError (ErrDuplicate) if the field already
// exists.
func (d *Dynamic) AddFieldString(name string, value string, meta *Attributes) error {
	cValue := C.CString(value)
	defer C.free(unsafe.Pointer(cValue))
	cMeta, freeMeta := attrsToC(meta)
	defer freeMeta()
	rc := C.bison_add_field_string(d.handle, cKey(name), cValue, cMeta)
	return checkBison(rc, fmt.Sprintf("add_field_string[%s]", name))
}

// AddFieldKey declares a new bison::key_t-valued field with optional
// documentation metadata -- the AddField counterpart to SetKey, for the
// same reason AddField itself can't dispatch to this from a plain int32.
// value is either an already-hashed uint32 or a string name, hashed the
// same way SetKey does. Fails with BisonError (ErrDuplicate) if the field
// already exists.
func (d *Dynamic) AddFieldKey(name string, value interface{}, meta *Attributes) error {
	v, err := resolveKeyValue(value)
	if err != nil {
		return err
	}
	cMeta, freeMeta := attrsToC(meta)
	defer freeMeta()
	rc := C.bison_add_field_key(d.handle, cKey(name), C.bison_hash(v), cMeta)
	return checkBison(rc, fmt.Sprintf("add_field_key[%s]", name))
}

// AddField declares a new field with optional documentation metadata,
// dispatching on value's concrete type the same way Set does (bool, int32/
// int, float32, string, []bool, []int32, []float32, []byte). Unlike Set
// this fails with BisonError (ErrDuplicate) if the field already exists.
// *Dynamic and key_t values are rejected -- bison_c.h has no
// bison_add_field_object, and a key_t field must go through AddFieldKey for
// the same reason SetKey is separate from Set.
func (d *Dynamic) AddField(name string, value interface{}, meta *Attributes) error {
	switch v := value.(type) {
	case bool:
		return d.AddFieldBool(name, v, meta)
	case int32:
		return d.AddFieldInt(name, v, meta)
	case int:
		i32, err := clampInt(v)
		if err != nil {
			return err
		}
		return d.AddFieldInt(name, i32, meta)
	case float32:
		return d.AddFieldFloat(name, v, meta)
	case string:
		return d.AddFieldString(name, v, meta)
	case []byte:
		return d.AddFieldVectorBytes(name, v, meta)
	case []bool:
		return d.AddFieldVectorBool(name, v, meta)
	case []int32:
		return d.AddFieldVectorInt(name, v, meta)
	case []float32:
		return d.AddFieldVectorFloat(name, v, meta)
	default:
		return fmt.Errorf("bison: unsupported value type for AddField: %T", value)
	}
}

// ── Serialization ────────────────────────────────────────────────────────

// ToJSON serializes to a JSON string. Pass indent = -1 for compact output.
// Field keys are emitted as "#<decimal>" (no key-name map is available
// through the C ABI).
func (d *Dynamic) ToJSON(indent int) (string, error) {
	var out *C.char
	rc := C.bison_to_json(d.handle, C.int(indent), &out)
	if err := checkBison(rc, "to_json"); err != nil {
		return "", err
	}
	defer C.bison_free_string(out)
	return C.GoString(out), nil
}

// ToYAML serializes to a YAML string. Same field-key limitation as ToJSON.
func (d *Dynamic) ToYAML() (string, error) {
	var out *C.char
	rc := C.bison_to_yaml(d.handle, &out)
	if err := checkBison(rc, "to_yaml"); err != nil {
		return "", err
	}
	defer C.bison_free_string(out)
	return C.GoString(out), nil
}

// Pretty returns a human-readable representation (bison_print).
func (d *Dynamic) Pretty(multiline bool, indent string) (string, error) {
	cIndent := C.CString(indent)
	defer C.free(unsafe.Pointer(cIndent))
	var ml C.int
	if multiline {
		ml = 1
	}
	opts := C.bison_print_options{multiline: ml, indent: cIndent}
	var out *C.char
	rc := C.bison_print(d.handle, &opts, &out)
	if err := checkBison(rc, "pretty"); err != nil {
		return "", err
	}
	defer C.bison_free_string(out)
	return C.GoString(out), nil
}

// Serialize encodes this object to the compact binary wire format (see
// FORMAT.md) -- the counterpart to Deserialize. Field keys are encoded as
// their raw hash, so (unlike ToJSON/ToYAML) this format is self-contained
// and needs no key-name map to round-trip.
func (d *Dynamic) Serialize() ([]byte, error) {
	var out *C.uint8_t
	var outLen C.size_t
	rc := C.bison_serialize(d.handle, &out, &outLen)
	if err := checkBison(rc, "serialize"); err != nil {
		return nil, err
	}
	defer C.bison_free_buffer(out)
	return C.GoBytes(unsafe.Pointer(out), C.int(outLen)), nil
}

// ─── Free functions / class registry ────────────────────────────────────────

// FromJSON parses a JSON string and returns the root object.
func FromJSON(text string) (*Dynamic, error) {
	cText := C.CString(text)
	defer C.free(unsafe.Pointer(cText))
	h := C.bison_from_json(cText)
	if h == nil {
		return nil, fmt.Errorf("bison: from_json: invalid or unsupported JSON")
	}
	return newOwned(h), nil
}

// FromYAML parses a YAML string and returns the root object.
func FromYAML(text string) (*Dynamic, error) {
	cText := C.CString(text)
	defer C.free(unsafe.Pointer(cText))
	h := C.bison_from_yaml(cText)
	if h == nil {
		return nil, fmt.Errorf("bison: from_yaml: invalid or unsupported YAML")
	}
	return newOwned(h), nil
}

// Deserialize decodes a buffer produced by Dynamic.Serialize.
func Deserialize(data []byte) (*Dynamic, error) {
	var ptr *C.uint8_t
	if len(data) > 0 {
		ptr = (*C.uint8_t)(unsafe.Pointer(&data[0]))
	}
	var h C.bison_handle
	rc := C.bison_deserialize(ptr, C.size_t(len(data)), &h)
	if err := checkBison(rc, "deserialize"); err != nil {
		return nil, err
	}
	return newOwned(h), nil
}

// registeredPrototypes keeps every class prototype ever registered via
// AddClass reachable (and hence its AddMethod trampolines alive) for as
// long as the class stays in the registry -- see AddMethod's doc comment.
var (
	registryMu           sync.Mutex
	registeredPrototypes []*Dynamic
)

// AddClass registers prototype (whose __class field is already set, e.g.
// via New) as a class in the global (or named) namespace registry.
// prototype must not be Closed by the caller for as long as the class stays
// registered -- AddClass keeps its own reference (see the registry note
// above) but does not take ownership away from the caller.
func AddClass(prototype *Dynamic, parentName, nsName string, meta *Attributes) error {
	cMeta, freeMeta := attrsToC(meta)
	defer freeMeta()
	rc := C.bison_add_class(cKeyOrZero(nsName), prototype.handle, cKeyOrZero(parentName), cMeta)
	if err := checkBison(rc, fmt.Sprintf("add_class(%q)", parentName)); err != nil {
		return err
	}
	registryMu.Lock()
	registeredPrototypes = append(registeredPrototypes, prototype)
	registryMu.Unlock()
	return nil
}

// FindClass looks up a registered class prototype. Returns a non-owning
// view (do not Close it), or (nil, nil) if not found.
func FindClass(klassName, nsName string) (*Dynamic, error) {
	h := C.bison_find_class(cKeyOrZero(nsName), cKey(klassName))
	if h == nil {
		return nil, nil
	}
	return newView(h), nil
}

// Instantiate creates a new instance of a registered class (with inherited
// fields and methods).
func Instantiate(klassName, nsName string) (*Dynamic, error) {
	h := C.bison_instantiate(cKeyOrZero(nsName), cKey(klassName))
	if h == nil {
		return nil, newBisonError(ErrNotFound, fmt.Sprintf("instantiate(%q)", klassName))
	}
	return newOwned(h), nil
}

// ClearRegistry removes all registered classes from the global registry,
// closing every prototype AddClass had kept alive.
func ClearRegistry() {
	C.bison_clear_registry()
	registryMu.Lock()
	old := registeredPrototypes
	registeredPrototypes = nil
	registryMu.Unlock()
	for _, p := range old {
		_ = p.Close()
	}
}

// ClassAttributes reads the class-level attribute metadata of a registered
// class.
func ClassAttributes(klassName, nsName string) (Attributes, error) {
	var c C.bison_attributes
	rc := C.bison_get_class_attributes(cKeyOrZero(nsName), cKey(klassName), &c)
	if err := checkBison(rc, fmt.Sprintf("class_attributes(%q)", klassName)); err != nil {
		return Attributes{}, err
	}
	return attrsFromC(c), nil
}

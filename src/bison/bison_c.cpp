// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

/**
 * @file bison_c.cpp
 * @brief C++ implementation of the pure-C Bison shared-library API.
 *
 * Each exported function wraps one or more calls into the C++ bison library.
 * All C++ exceptions are caught at the boundary and converted to `bison_error`
 * return codes so that no exception can propagate through the C ABI.
 *
 * ### Handle representation
 * A `bison_handle` is a pointer to a heap-allocated
 * `std::shared_ptr<bdg::bison::dynamic>`.  This keeps the ref-counting
 * transparent: `bison_add_ref` allocates a *new* `shared_ptr` that shares
 * ownership with the original; `bison_release` deletes the `shared_ptr`,
 * decrementing the underlying object's ref-count (and destroying it when it
 * reaches zero).
 *
 * Non-owning handles returned by `bison_find_class` are raw pointers wrapped
 * in a sentinel struct so callers cannot accidentally call `bison_release` on
 * them (doing so on a non-owning handle would be a double-free).  In practice
 * the C API documents this contract clearly and the type is the same opaque
 * `bison_handle`; callers must follow the documented ownership rules.
 */

#include "../include/bison_c.h"
#include "bison.hpp"

#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

// ─── Internal helpers ──────────────────────────────────────────────────────

// Each *owning* handle is a heap-allocated shared_ptr<dynamic>.
using sp_dyn = std::shared_ptr<bdg::bison::dynamic>;

/** Cast an opaque bison_handle back to a shared_ptr<dynamic>*. */
static inline sp_dyn* as_sp(bison_handle h) {
  return reinterpret_cast<sp_dyn*>(h);
}

/** Cast a shared_ptr<dynamic>* to the opaque handle type. */
static inline bison_handle as_handle(sp_dyn* p) {
  return reinterpret_cast<bison_handle>(p);
}

/** Dereference a handle to the underlying dynamic object.  Returns nullptr if h
 * is null or the shared_ptr is empty. */
static inline bdg::bison::dynamic* dyn(bison_handle h) {
  if (!h)
    return nullptr;
  return as_sp(h)->get();
}

// ─── Lifecycle ─────────────────────────────────────────────────────────────

BISON_API bison_handle bison_create(bison_hash klass_name) {
  try {
    auto* sp = new sp_dyn(std::make_shared<bdg::bison::dynamic>(bdg::bison::key_t{klass_name}));
    return as_handle(sp);
  } catch (...) {
    return nullptr;
  }
}

BISON_API bison_handle bison_instantiate(bison_hash ns_name, bison_hash klass_name) {
  try {
    bdg::bison::dynamic obj =
        bdg::bison::dynamic::instantiate(bdg::bison::key_t{ns_name}, bdg::bison::key_t{klass_name});
    auto* sp = new sp_dyn(std::make_shared<bdg::bison::dynamic>(std::move(obj)));
    return as_handle(sp);
  } catch (...) {
    return nullptr;
  }
}

BISON_API bison_handle bison_add_ref(bison_handle h) {
  if (!h)
    return nullptr;
  try {
    auto* original = as_sp(h);
    auto* copy = new sp_dyn(*original); // increments shared_ptr refcount
    return as_handle(copy);
  } catch (...) {
    return nullptr;
  }
}

BISON_API void bison_release(bison_handle h) {
  if (!h)
    return;
  delete as_sp(h); // decrements shared_ptr refcount; may destroy the object
}

BISON_API bison_handle bison_clone(bison_handle h) {
  if (!h)
    return nullptr;
  try {
    auto* sp = new sp_dyn(std::make_shared<bdg::bison::dynamic>(as_sp(h)->get()->clone()));
    return as_handle(sp);
  } catch (...) {
    return nullptr;
  }
}

// ─── Import helpers ─────────────────────────────────────────────────────────

BISON_API bison_handle bison_from_json(const char* json) {
  if (!json)
    return nullptr;
  try {
    auto sp = bdg::bison::extensions::from_json(json);
    return as_handle(new sp_dyn(std::move(sp)));
  } catch (...) {
    return nullptr;
  }
}

BISON_API bison_handle bison_from_yaml(const char* yaml) {
  if (!yaml)
    return nullptr;
  try {
    auto sp = bdg::bison::extensions::from_yaml(yaml);
    return as_handle(new sp_dyn(std::move(sp)));
  } catch (...) {
    return nullptr;
  }
}

// ─── Export helpers ──────────────────────────────────────────────────────────

BISON_API bison_error bison_to_json(bison_handle h, int indent, char** out) {
  if (!h || !out)
    return BISON_ERR_NULL;
  try {
    const std::string result = bdg::bison::extensions::to_json(*dyn(h), {}, indent);
    *out = new char[result.size() + 1];
    std::memcpy(*out, result.c_str(), result.size() + 1);
    return BISON_OK;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error bison_to_yaml(bison_handle h, char** out) {
  if (!h || !out)
    return BISON_ERR_NULL;
  try {
    const std::string result = bdg::bison::extensions::to_yaml(*dyn(h), {});
    *out = new char[result.size() + 1];
    std::memcpy(*out, result.c_str(), result.size() + 1);
    return BISON_OK;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

// ─── Pretty-print ────────────────────────────────────────────────────────────

BISON_API bison_error bison_print(bison_handle h, const bison_print_options* opts, char** out) {
  if (!h || !out)
    return BISON_ERR_NULL;
  try {
    bdg::bison::print_options cpp_opts;
    if (opts) {
      cpp_opts.multiline = opts->multiline != 0;
      if (opts->indent)
        cpp_opts.indent = opts->indent;
    }
    const std::string result = bdg::bison::print(*dyn(h), cpp_opts);
    *out = new char[result.size() + 1];
    std::memcpy(*out, result.c_str(), result.size() + 1);
    return BISON_OK;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API void bison_free_string(char* s) {
  delete[] s;
}

// ─── Class registry ─────────────────────────────────────────────────────────

// Build a bison attribute vector from a C bison_attributes struct.
static std::vector<std::shared_ptr<const bdg::bison::attribute>> attrs_from_meta(const bison_attributes* meta) {
  std::vector<std::shared_ptr<const bdg::bison::attribute>> attrs;
  if (!meta)
    return attrs;
  if (meta->display_name)
    attrs.push_back(bdg::bison::attr<bdg::bison::DisplayName>(meta->display_name));
  if (meta->description)
    attrs.push_back(bdg::bison::attr<bdg::bison::Description>(meta->description));
  if (meta->category)
    attrs.push_back(bdg::bison::attr<bdg::bison::Category>(meta->category));
  if (meta->obsolete) {
    std::string msg = meta->obsolete_message ? meta->obsolete_message : "";
    attrs.push_back(bdg::bison::attr<bdg::bison::Obsolete>(std::move(msg)));
  }
  if (meta->required)
    attrs.push_back(bdg::bison::attr<bdg::bison::Required>());
  return attrs;
}

BISON_API bison_error
bison_add_class(bison_hash ns_name, bison_handle klass, bison_hash parent_name, const bison_attributes* meta) {
  if (!klass)
    return BISON_ERR_NULL;
  try {
    sp_dyn copy = *as_sp(klass);
    bool ok = bdg::bison::dynamic::addClass(
        bdg::bison::key_t{ns_name}, std::move(copy), bdg::bison::key_t{parent_name}, attrs_from_meta(meta));
    return ok ? BISON_OK : BISON_ERR_DUPLICATE;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_handle bison_find_class(bison_hash ns_name, bison_hash klass_name) {
  try {
    auto lp = bdg::bison::dynamic::getRegistry().rlock();
    auto nsIt = lp->find(bdg::bison::key_t{ns_name});
    if (nsIt == lp->end())
      return nullptr;
    auto it = nsIt->second.find(bdg::bison::key_t{klass_name});
    if (it == nsIt->second.end())
      return nullptr;
    auto* sp = new sp_dyn(it->second); // owning copy from registry
    return as_handle(sp);
  } catch (...) {
    return nullptr;
  }
}

BISON_API void bison_clear_registry(void) {
  bdg::bison::dynamic::getRegistry().wlock()->clear();
}

// Fill a bison_attributes struct from the attributes on a bison::field.
static void fill_attrs(bison_attributes* out, const bdg::bison::field& f) {
  *out = bison_attributes{};
  if (auto* dn = f.findAttribute<bdg::bison::DisplayName>())
    out->display_name = dn->name().c_str();
  if (auto* d = f.findAttribute<bdg::bison::Description>())
    out->description = d->text().c_str();
  if (auto* c = f.findAttribute<bdg::bison::Category>())
    out->category = c->name().c_str();
  if (auto* o = f.findAttribute<bdg::bison::Obsolete>()) {
    out->obsolete = 1;
    out->obsolete_message = o->message().empty() ? nullptr : o->message().c_str();
  }
  if (f.findAttribute<bdg::bison::Required>())
    out->required = 1;
}

BISON_API bison_error bison_get_class_attributes(bison_hash ns_name, bison_hash klass_name, bison_attributes* out) {
  if (!out)
    return BISON_ERR_NULL;
  try {
    auto lp = bdg::bison::dynamic::getRegistry().rlock();
    auto nsIt = lp->find(bdg::bison::key_t{ns_name});
    if (nsIt == lp->end())
      return BISON_ERR_NOT_FOUND;
    auto it = nsIt->second.find(bdg::bison::key_t{klass_name});
    if (it == nsIt->second.end())
      return BISON_ERR_NOT_FOUND;
    const auto* f = it->second->findField(bdg::bison::dynamic::CLASS);
    if (!f) {
      *out = bison_attributes{};
      return BISON_OK;
    }
    fill_attrs(out, *f);
    return BISON_OK;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error bison_get_field_attributes(bison_handle h, bison_hash field_key, bison_attributes* out) {
  if (!h || !out)
    return BISON_ERR_NULL;
  try {
    const auto* f = dyn(h)->findField(bdg::bison::key_t{field_key});
    if (!f)
      return BISON_ERR_NOT_FOUND;
    fill_attrs(out, *f);
    return BISON_OK;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error bison_get_method_attributes(bison_handle h, bison_hash method_key, bison_attributes* out) {
  if (!h || !out)
    return BISON_ERR_NULL;
  try {
    const auto* e = dyn(h)->findMethod(bdg::bison::key_t{method_key});
    if (!e)
      return BISON_ERR_NOT_FOUND;
    *out = bison_attributes{};
    if (auto* dn = e->findAttribute<bdg::bison::DisplayName>())
      out->display_name = dn->name().c_str();
    if (auto* d = e->findAttribute<bdg::bison::Description>())
      out->description = d->text().c_str();
    if (auto* c = e->findAttribute<bdg::bison::Category>())
      out->category = c->name().c_str();
    if (auto* o = e->findAttribute<bdg::bison::Obsolete>()) {
      out->obsolete = 1;
      out->obsolete_message = o->message().empty() ? nullptr : o->message().c_str();
    }
    if (e->findAttribute<bdg::bison::Required>())
      out->required = 1;
    return BISON_OK;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

// ─── Field registration ──────────────────────────────────────────────────────

static bison_error add_field_impl(bison_handle obj, bison_hash key, bdg::bison::field f) {
  if (!obj)
    return BISON_ERR_NULL;
  try {
    bool ok = dyn(obj)->addField(bdg::bison::key_t{key}, std::move(f));
    return ok ? BISON_OK : BISON_ERR_DUPLICATE;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error
bison_add_field_int(bison_handle obj, bison_hash key, int32_t value, const bison_attributes* meta) {
  bdg::bison::field f{value};
  for (auto& a : attrs_from_meta(meta))
    f.addAttribute(std::move(a));
  return add_field_impl(obj, key, std::move(f));
}

BISON_API bison_error
bison_add_field_float(bison_handle obj, bison_hash key, float value, const bison_attributes* meta) {
  bdg::bison::field f{value};
  for (auto& a : attrs_from_meta(meta))
    f.addAttribute(std::move(a));
  return add_field_impl(obj, key, std::move(f));
}

BISON_API bison_error bison_add_field_bool(bison_handle obj, bison_hash key, int value, const bison_attributes* meta) {
  bdg::bison::field f{bool(value != 0)};
  for (auto& a : attrs_from_meta(meta))
    f.addAttribute(std::move(a));
  return add_field_impl(obj, key, std::move(f));
}

BISON_API bison_error
bison_add_field_string(bison_handle obj, bison_hash key, const char* value, const bison_attributes* meta) {
  if (!value)
    return BISON_ERR_NULL;
  bdg::bison::field f{std::string{value}};
  for (auto& a : attrs_from_meta(meta))
    f.addAttribute(std::move(a));
  return add_field_impl(obj, key, std::move(f));
}

BISON_API bison_error
bison_add_field_key(bison_handle obj, bison_hash key, bison_hash value, const bison_attributes* meta) {
  bdg::bison::field f{bdg::bison::key_t{value}};
  for (auto& a : attrs_from_meta(meta))
    f.addAttribute(std::move(a));
  return add_field_impl(obj, key, std::move(f));
}

BISON_API bison_error bison_add_field_vector_bool(
    bison_handle obj, bison_hash key, const int* values, size_t count, const bison_attributes* meta) {
  if (count > 0 && !values)
    return BISON_ERR_NULL;
  std::vector<bool> v;
  v.reserve(count);
  for (size_t i = 0; i < count; ++i)
    v.push_back(values[i] != 0);
  bdg::bison::field f{std::move(v)};
  for (auto& a : attrs_from_meta(meta))
    f.addAttribute(std::move(a));
  return add_field_impl(obj, key, std::move(f));
}

BISON_API bison_error bison_add_field_vector_int(
    bison_handle obj, bison_hash key, const int32_t* values, size_t count, const bison_attributes* meta) {
  if (count > 0 && !values)
    return BISON_ERR_NULL;
  bdg::bison::field f{std::vector<int32_t>{values, values + count}};
  for (auto& a : attrs_from_meta(meta))
    f.addAttribute(std::move(a));
  return add_field_impl(obj, key, std::move(f));
}

BISON_API bison_error bison_add_field_vector_float(
    bison_handle obj, bison_hash key, const float* values, size_t count, const bison_attributes* meta) {
  if (count > 0 && !values)
    return BISON_ERR_NULL;
  bdg::bison::field f{std::vector<float>{values, values + count}};
  for (auto& a : attrs_from_meta(meta))
    f.addAttribute(std::move(a));
  return add_field_impl(obj, key, std::move(f));
}

BISON_API bison_error bison_add_field_vector_bytes(
    bison_handle obj, bison_hash key, const uint8_t* values, size_t count, const bison_attributes* meta) {
  if (count > 0 && !values)
    return BISON_ERR_NULL;
  bdg::bison::field f{std::vector<uint8_t>{values, values + count}};
  for (auto& a : attrs_from_meta(meta))
    f.addAttribute(std::move(a));
  return add_field_impl(obj, key, std::move(f));
}

// ─── Setters ────────────────────────────────────────────────────────────────

BISON_API bison_error bison_set_int(bison_handle h, bison_hash name, int32_t value) {
  if (!h)
    return BISON_ERR_NULL;
  try {
    (*dyn(h))[bdg::bison::key_t{name}] = value;
    return BISON_OK;
  } catch (const std::runtime_error&) {
    return BISON_ERR_TYPE;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error bison_set_float(bison_handle h, bison_hash name, float value) {
  if (!h)
    return BISON_ERR_NULL;
  try {
    (*dyn(h))[bdg::bison::key_t{name}] = value;
    return BISON_OK;
  } catch (const std::runtime_error&) {
    return BISON_ERR_TYPE;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error bison_set_bool(bison_handle h, bison_hash name, int value) {
  if (!h)
    return BISON_ERR_NULL;
  try {
    (*dyn(h))[bdg::bison::key_t{name}] = bool(value != 0);
    return BISON_OK;
  } catch (const std::runtime_error&) {
    return BISON_ERR_TYPE;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error bison_set_string(bison_handle h, bison_hash name, const char* value) {
  if (!h || !value)
    return BISON_ERR_NULL;
  try {
    (*dyn(h))[bdg::bison::key_t{name}] = std::string(value);
    return BISON_OK;
  } catch (const std::runtime_error&) {
    return BISON_ERR_TYPE;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error bison_set_key(bison_handle h, bison_hash name, bison_hash value) {
  if (!h)
    return BISON_ERR_NULL;
  try {
    (*dyn(h))[bdg::bison::key_t{name}] = bdg::bison::key_t{value};
    return BISON_OK;
  } catch (const std::runtime_error&) {
    return BISON_ERR_TYPE;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error bison_set_object(bison_handle h, bison_hash name, bison_handle value) {
  if (!h)
    return BISON_ERR_NULL;
  try {
    // value == nullptr means set a null dynamic ref.
    sp_dyn child = value ? *as_sp(value) : sp_dyn{};
    (*dyn(h))[bdg::bison::key_t{name}] = child;
    return BISON_OK;
  } catch (const std::runtime_error&) {
    return BISON_ERR_TYPE;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error bison_set_int_at(bison_handle h, size_t index, int32_t value) {
  return bison_set_int(h, static_cast<bison_hash>(index), value);
}

BISON_API bison_error bison_set_float_at(bison_handle h, size_t index, float value) {
  return bison_set_float(h, static_cast<bison_hash>(index), value);
}

BISON_API bison_error bison_set_string_at(bison_handle h, size_t index, const char* value) {
  return bison_set_string(h, static_cast<bison_hash>(index), value);
}

// ─── Getters ────────────────────────────────────────────────────────────────

BISON_API bison_error bison_get_int(bison_handle h, bison_hash name, int32_t* out) {
  if (!h || !out)
    return BISON_ERR_NULL;
  try {
    *out = (*dyn(h))[bdg::bison::key_t{name}].as<int32_t>();
    return BISON_OK;
  } catch (const std::runtime_error&) {
    return BISON_ERR_TYPE;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error bison_get_float(bison_handle h, bison_hash name, float* out) {
  if (!h || !out)
    return BISON_ERR_NULL;
  try {
    *out = (*dyn(h))[bdg::bison::key_t{name}].as<float>();
    return BISON_OK;
  } catch (const std::runtime_error&) {
    return BISON_ERR_TYPE;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error bison_get_bool(bison_handle h, bison_hash name, int* out) {
  if (!h || !out)
    return BISON_ERR_NULL;
  try {
    *out = (*dyn(h))[bdg::bison::key_t{name}].as<bool>() ? 1 : 0;
    return BISON_OK;
  } catch (const std::runtime_error&) {
    return BISON_ERR_TYPE;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error bison_get_string(bison_handle h, bison_hash name, char* buf, size_t buf_len, size_t* len_out) {
  if (!h)
    return BISON_ERR_NULL;
  try {
    const std::string& s = (*dyn(h))[bdg::bison::key_t{name}].as<std::string>();
    if (len_out)
      *len_out = s.size();
    if (buf && buf_len > 0) {
      size_t copy_len = s.size() < buf_len - 1 ? s.size() : buf_len - 1;
      std::memcpy(buf, s.data(), copy_len);
      buf[copy_len] = '\0';
    }
    return BISON_OK;
  } catch (const std::runtime_error&) {
    return BISON_ERR_TYPE;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error bison_get_key(bison_handle h, bison_hash name, bison_hash* out) {
  if (!h || !out)
    return BISON_ERR_NULL;
  try {
    *out = static_cast<bison_hash>((*dyn(h))[bdg::bison::key_t{name}].as<bdg::bison::key_t>().id);
    return BISON_OK;
  } catch (const std::runtime_error&) {
    return BISON_ERR_TYPE;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error bison_get_object(bison_handle h, bison_hash name, bison_handle* out) {
  if (!h || !out)
    return BISON_ERR_NULL;
  try {
    auto sp = (*dyn(h))[bdg::bison::key_t{name}].as<bdg::bison::dynamic_ptr>();
    if (!sp) {
      *out = nullptr;
      return BISON_OK;
    }
    *out = as_handle(new sp_dyn(sp));
    return BISON_OK;
  } catch (const std::runtime_error&) {
    return BISON_ERR_TYPE;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error bison_get_int_at(bison_handle h, size_t index, int32_t* out) {
  return bison_get_int(h, static_cast<bison_hash>(index), out);
}

BISON_API bison_error bison_get_float_at(bison_handle h, size_t index, float* out) {
  return bison_get_float(h, static_cast<bison_hash>(index), out);
}

BISON_API bison_error bison_get_string_at(bison_handle h, size_t index, char* buf, size_t buf_len, size_t* len_out) {
  return bison_get_string(h, static_cast<bison_hash>(index), buf, buf_len, len_out);
}

BISON_API size_t bison_size(bison_handle h) {
  if (!h)
    return 0;
  try {
    return dyn(h)->size();
  } catch (...) {
    return 0;
  }
}

BISON_API bison_hash bison_key(const char* name) {
  if (!name)
    return 0u;
  try {
    return static_cast<bison_hash>(bdg::bison::hash(name));
  } catch (...) {
    return 0u;
  }
}

// ─── Methods ────────────────────────────────────────────────────────────────

BISON_API bison_error
bison_add_method(bison_handle h, bison_hash name, bison_method_fn fn, void* user, const bison_attributes* meta) {
  if (!h || !fn)
    return BISON_ERR_NULL;
  try {
    struct closure_t {
      bison_method_fn fn;
      void* user;

      closure_t(bison_method_fn f, void* u) : fn(f), user(u) {}
      closure_t(const closure_t&) = delete;
      closure_t& operator=(const closure_t&) = delete;
    };
    auto cl = std::make_shared<closure_t>(fn, user);

    bdg::bison::method_fn wrapped =
        [cl](bdg::bison::dynamic& self, const bdg::bison::dynamic& params) -> bdg::bison::dynamic {
      // Create a non-owning handle for self using a no-op deleter.
      // We wrap &self (a C++ reference to the actual dynamic) in a
      // shared_ptr that does NOT delete the object when destroyed, so
      // the C callback can read and mutate self in-place through the
      // handle without any extra copy/propagation step.
      auto* self_sp = new sp_dyn(&self, [](bdg::bison::dynamic*) {});
      // params is const; make a heap copy so the callback can hold a handle.
      auto* param_sp = new sp_dyn(std::make_shared<bdg::bison::dynamic>(params));
      // result is a fresh empty dynamic; caller populates it.
      auto result_dyn = std::make_shared<bdg::bison::dynamic>();
      auto* res_sp = new sp_dyn(result_dyn);

      cl->fn(as_handle(self_sp), as_handle(param_sp), as_handle(res_sp), cl->user);

      bdg::bison::dynamic result = std::move(*result_dyn);

      delete self_sp;
      delete param_sp;
      delete res_sp;

      return result;
    };

    bool ok = dyn(h)->addMethod(bdg::bison::key_t{name}, bdg::bison::method{std::move(wrapped), attrs_from_meta(meta)});
    return ok ? BISON_OK : BISON_ERR_DUPLICATE;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

BISON_API bison_error bison_call(bison_handle h, bison_hash name, bison_handle params, bison_handle* result) {
  if (!h || !params || !result)
    return BISON_ERR_NULL;
  try {
    bdg::bison::dynamic ret = dyn(h)->call(bdg::bison::key_t{name}, *dyn(params));
    *result = as_handle(new sp_dyn(std::make_shared<bdg::bison::dynamic>(std::move(ret))));
    return BISON_OK;
  } catch (const std::runtime_error& e) {
    std::string msg(e.what());
    if (msg.find("not found") != std::string::npos || msg.find("Not found") != std::string::npos)
      return BISON_ERR_NOT_FOUND;
    return BISON_ERR_EXCEPTION;
  } catch (...) {
    return BISON_ERR_EXCEPTION;
  }
}

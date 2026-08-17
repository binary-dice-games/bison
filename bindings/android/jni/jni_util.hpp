// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

/**
 * @file jni_util.hpp
 * @brief Shared helpers for the Android JNI glue (`bison_jni`, `rmi_jni`).
 *
 * Every `Java_*` export in this directory follows the same shape: pull
 * primitives/strings out of JNI arguments, call one `bison_c.h`/`rmi_c.h`
 * function, and either return a primitive/handle or throw the matching Java
 * exception. These helpers factor out the two repetitive parts -- jstring
 * conversion and error-code-to-exception translation -- so each `Java_*`
 * function body stays a couple of lines, mirroring how thin the Python
 * `ctypes` and C# `[LibraryImport]` binding layers are (see
 * docs/bindings.md).
 */

#ifndef BISON_ANDROID_JNI_UTIL_HPP
#define BISON_ANDROID_JNI_UTIL_HPP

#include <jni.h>

#include <string>

#include "bison_c.h"
#include "rmi_c.h"

namespace bdg::bison::jni {

// ── Shared JavaVM / cached class & method IDs ──────────────────────────────
// Populated once by bison_jni.cpp's JNI_OnLoad (the only JNI_OnLoad in this
// shared library) and consumed from both bison_jni.cpp and rmi_jni.cpp.
// FindClass()/GetMethodID() are only reliable when called on a thread with
// Java frames on its stack (e.g. inside JNI_OnLoad, or a Java-initiated
// call); an RMI worker thread invoking a BisonMethod/ProxyEvent/AuthHandler
// callback has neither, so the lookups are done once here and reused as
// global refs -- the standard fix for that gotcha.
extern JavaVM* g_jvm;
extern jclass g_dynamic_class;
extern jmethodID g_dynamic_wrap_borrowed;
extern jclass g_attributes_class;
extern jmethodID g_attributes_ctor;
extern jfieldID g_attributes_display_name;
extern jfieldID g_attributes_description;
extern jfieldID g_attributes_category;
extern jfieldID g_attributes_obsolete;
extern jfieldID g_attributes_obsolete_message;
extern jfieldID g_attributes_required;
extern jclass g_proxy_event_class;
extern jmethodID g_proxy_event_on_event;
extern jclass g_auth_handler_class;
extern jmethodID g_auth_handler_authenticate;

/// Attaches the current native thread to `g_jvm` if it isn't already a Java
/// thread (e.g. an RMI worker thread), and detaches again on scope exit.
/// Used by every JNI upcall trampoline (`BisonMethod`, `ProxyEvent`,
/// `AuthHandler`) that may run off a JVM-created thread.
class jni_env_guard {
public:
  jni_env_guard();
  ~jni_env_guard();
  jni_env_guard(const jni_env_guard&) = delete;
  jni_env_guard& operator=(const jni_env_guard&) = delete;

  /// `nullptr` if attaching failed; callers must check before use.
  JNIEnv* env() const { return env_; }

private:
  JNIEnv* env_ = nullptr;
  bool attached_ = false;
};

/// RAII wrapper turning a jstring into a UTF-8 std::string; safe to pass a
/// null jstring (produces an empty string), matching how the Python/C#
/// bindings treat an omitted name.
class jstring_view {
public:
  jstring_view(JNIEnv* env, jstring s) : env_(env), s_(s) {
    if (s_) chars_ = env_->GetStringUTFChars(s_, nullptr);
  }
  ~jstring_view() {
    if (s_ && chars_) env_->ReleaseStringUTFChars(s_, chars_);
  }
  jstring_view(const jstring_view&) = delete;
  jstring_view& operator=(const jstring_view&) = delete;

  const char* c_str() const { return chars_ ? chars_ : ""; }

private:
  JNIEnv* env_;
  jstring s_;
  const char* chars_ = nullptr;
};

inline jstring to_jstring(JNIEnv* env, const char* s) {
  return s ? env->NewStringUTF(s) : nullptr;
}

/// Builds a `bison_attributes` pointing at borrowed UTF-8 buffers owned by
/// this object; keep it alive for the duration of the native call it feeds.
/// `nullptr` @p meta_obj (no metadata supplied) yields a null `bison_attributes*`.
class attributes_view {
public:
  attributes_view(JNIEnv* env, jobject meta_obj);

  const bison_attributes* ptr() const { return has_value_ ? &attrs_ : nullptr; }

private:
  bison_attributes attrs_{};
  bool has_value_ = false;
  jstring_view display_name_;
  jstring_view description_;
  jstring_view category_;
  jstring_view obsolete_message_;
};

/// Constructs a new `com.bdg.bison.Attributes` from a `bison_attributes`
/// (the counterpart to `attributes_view`, for `bison_get_*_attributes()`
/// results). Returns `nullptr` on failure with a pending JNI exception.
jobject new_attributes(JNIEnv* env, const bison_attributes& attrs);

/// Throws `com.bdg.bison.BisonException(code, message)` in @p env. Callers
/// must return to Java immediately afterwards -- C++ execution continues
/// past a pending JNI exception until the native frame returns.
void throw_bison_exception(JNIEnv* env, bison_error err);

/// Throws `com.bdg.bison.rmi.RmiException(code, message)` in @p env.
void throw_rmi_exception(JNIEnv* env, rmi_error err);

/// Logs a diagnostic that has nowhere else to go (e.g. a Java exception
/// thrown from inside a `BisonMethod` callback, which `bison_method_fn`'s
/// plain-C-function-pointer signature can't propagate). `__android_log_print`
/// on Android, stderr everywhere else (including this directory's own
/// host-JVM validation build) -- a narrow enough platform delta to keep
/// inline rather than a `_posix`/`_win`-style file split.
void log_error(const char* tag, const char* message);

/// Reinterprets a Java `long` handle field as the ABI handle type `H`.
/// Bison ABI handles are opaque pointers, so this is a bit-preserving cast,
/// not a numeric conversion -- the same representation the C# binding's
/// `nint`-typed `[LibraryImport]` handles use.
template <typename H>
H from_jlong(jlong v) {
  return reinterpret_cast<H>(static_cast<intptr_t>(v));
}

template <typename H>
jlong to_jlong(H h) {
  return static_cast<jlong>(reinterpret_cast<intptr_t>(h));
}

}  // namespace bdg::bison::jni

#endif  // BISON_ANDROID_JNI_UTIL_HPP

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

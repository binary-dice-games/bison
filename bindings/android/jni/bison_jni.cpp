// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

/**
 * @file bison_jni.cpp
 * @brief JNI glue for `com.bdg.bison.{Key,Dynamic,BisonMethod}`.
 *
 * Every `Java_*` export here does the same three things: unpack JNI
 * arguments into C types, call one `bison_c.h` function, and either return
 * a value or throw `BisonException` via `throw_bison_exception()` -- the
 * same shape `src/bison/bison_c.cpp` uses to wrap the internal C++ API, one
 * layer further out. See docs/bindings.md for the binding's API coverage
 * and documented gaps.
 */

#include <jni.h>

#include <cstring>
#include <vector>

#include "bison_c.h"
#include "jni_util.hpp"

using namespace bdg::bison::jni;

// ── JavaVM / cached class & method IDs (populated in JNI_OnLoad) ──────────
// FindClass()/GetMethodID() are only reliable when called on a thread with
// Java frames on its stack (e.g. inside JNI_OnLoad, or a Java-initiated
// call); an RMI worker thread invoking a BisonMethod callback has neither,
// so the class/method lookups needed for that upcall are done once here and
// reused as global refs -- the standard fix for that gotcha.
namespace {

JavaVM* g_jvm = nullptr;
jclass g_dynamic_class = nullptr;
jmethodID g_dynamic_wrap_borrowed = nullptr;
jclass g_bison_method_class = nullptr;
jmethodID g_bison_method_invoke = nullptr;

/// Context captured for one `Dynamic.addMethod()` registration; leaked for
/// the process lifetime, matching bison_c.h's `bison_add_method`, which has
/// no unregister call to hang a destructor off of -- the same tradeoff the
/// C++/Python/C# bindings' method registrations make.
struct method_ctx {
  jobject callback; // global ref to the BisonMethod instance
};

void JNICALL method_trampoline(bison_handle self, bison_handle params, bison_handle result, void* user) {
  auto* ctx = static_cast<method_ctx*>(user);

  JNIEnv* env = nullptr;
  bool attached = false;
  if (g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
    if (g_jvm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr) != JNI_OK || !env) return;
    attached = true;
  }

  jobject j_self = env->CallStaticObjectMethod(g_dynamic_class, g_dynamic_wrap_borrowed, to_jlong(self));
  jobject j_params = env->CallStaticObjectMethod(g_dynamic_class, g_dynamic_wrap_borrowed, to_jlong(params));
  jobject j_result = env->CallStaticObjectMethod(g_dynamic_class, g_dynamic_wrap_borrowed, to_jlong(result));

  env->CallVoidMethod(ctx->callback, g_bison_method_invoke, j_self, j_params, j_result);
  // bison_method_fn cannot propagate an exception across the C ABI -- the
  // library treats this callback like any other C function pointer, not a
  // C++ call site it wraps in try/catch. A Java exception has nowhere to go
  // but be logged and dropped, matching how bison_c.cpp converts unexpected
  // exceptions into BISON_ERR_EXCEPTION rather than letting them escape.
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    log_error("bison_jni", "BisonMethod callback threw; the exception could not cross the C ABI and was dropped");
  }

  if (j_self) env->DeleteLocalRef(j_self);
  if (j_params) env->DeleteLocalRef(j_params);
  if (j_result) env->DeleteLocalRef(j_result);

  if (attached) g_jvm->DetachCurrentThread();
}

// Two-call-convention helper for bison_get_vector_*(h, name, buf, buf_len,
// &len): query the length with buf=NULL, then fetch into a right-sized
// buffer.
template <typename Elem, typename Fn>
std::vector<Elem> read_vector(bison_handle h, bison_hash name, Fn fn, JNIEnv* env, bool* ok) {
  size_t len = 0;
  bison_error err = fn(h, name, static_cast<Elem*>(nullptr), 0, &len);
  if (err != BISON_OK) {
    throw_bison_exception(env, err);
    *ok = false;
    return {};
  }
  std::vector<Elem> buf(len);
  if (len > 0) {
    err = fn(h, name, buf.data(), len, &len);
    if (err != BISON_OK) {
      throw_bison_exception(env, err);
      *ok = false;
      return {};
    }
  }
  *ok = true;
  return buf;
}

}  // namespace

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
  g_jvm = vm;
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;

  jclass dynamic_local = env->FindClass("com/bdg/bison/Dynamic");
  if (!dynamic_local) return JNI_ERR;
  g_dynamic_class = static_cast<jclass>(env->NewGlobalRef(dynamic_local));
  g_dynamic_wrap_borrowed =
      env->GetStaticMethodID(g_dynamic_class, "wrapBorrowed", "(J)Lcom/bdg/bison/Dynamic;");

  jclass method_local = env->FindClass("com/bdg/bison/BisonMethod");
  if (!method_local) return JNI_ERR;
  g_bison_method_class = static_cast<jclass>(env->NewGlobalRef(method_local));
  g_bison_method_invoke = env->GetMethodID(
      g_bison_method_class, "invoke",
      "(Lcom/bdg/bison/Dynamic;Lcom/bdg/bison/Dynamic;Lcom/bdg/bison/Dynamic;)V");

  return JNI_VERSION_1_6;
}

// ─── Key ────────────────────────────────────────────────────────────────

JNIEXPORT jint JNICALL Java_com_bdg_bison_Key_nativeKey(JNIEnv* env, jclass, jstring name) {
  jstring_view v(env, name);
  return static_cast<jint>(bison_key(v.c_str()));
}

// ─── Lifecycle ────────────────────────────────────────────────────────────

JNIEXPORT jlong JNICALL Java_com_bdg_bison_Dynamic_nativeCreate(JNIEnv*, jclass, jint class_name_hash) {
  return to_jlong(bison_create(static_cast<bison_hash>(class_name_hash)));
}

JNIEXPORT jlong JNICALL Java_com_bdg_bison_Dynamic_nativeFromJson(JNIEnv* env, jclass, jstring json) {
  jstring_view v(env, json);
  bison_handle h = bison_from_json(v.c_str());
  if (!h) throw_bison_exception(env, BISON_ERR_PARSE);
  return to_jlong(h);
}

JNIEXPORT jlong JNICALL Java_com_bdg_bison_Dynamic_nativeDeserialize(JNIEnv* env, jclass, jbyteArray data) {
  jsize len = data ? env->GetArrayLength(data) : 0;
  std::vector<uint8_t> buf(len);
  if (len > 0) env->GetByteArrayRegion(data, 0, len, reinterpret_cast<jbyte*>(buf.data()));
  bison_handle out = nullptr;
  bison_error err = bison_deserialize(buf.data(), static_cast<size_t>(len), &out);
  if (err != BISON_OK) {
    throw_bison_exception(env, err);
    return 0;
  }
  return to_jlong(out);
}

JNIEXPORT jlong JNICALL Java_com_bdg_bison_Dynamic_nativeClone(JNIEnv*, jclass, jlong handle) {
  return to_jlong(bison_clone(from_jlong<bison_handle>(handle)));
}

JNIEXPORT void JNICALL Java_com_bdg_bison_Dynamic_nativeRelease(JNIEnv*, jclass, jlong handle) {
  bison_release(from_jlong<bison_handle>(handle));
}

// ─── Scalar setters ─────────────────────────────────────────────────────

JNIEXPORT void JNICALL Java_com_bdg_bison_Dynamic_nativeSetInt(
    JNIEnv* env, jclass, jlong handle, jint name, jint value) {
  bison_error err = bison_set_int(from_jlong<bison_handle>(handle), name, value);
  if (err != BISON_OK) throw_bison_exception(env, err);
}

JNIEXPORT void JNICALL Java_com_bdg_bison_Dynamic_nativeSetFloat(
    JNIEnv* env, jclass, jlong handle, jint name, jfloat value) {
  bison_error err = bison_set_float(from_jlong<bison_handle>(handle), name, value);
  if (err != BISON_OK) throw_bison_exception(env, err);
}

JNIEXPORT void JNICALL Java_com_bdg_bison_Dynamic_nativeSetBool(
    JNIEnv* env, jclass, jlong handle, jint name, jboolean value) {
  bison_error err = bison_set_bool(from_jlong<bison_handle>(handle), name, value == JNI_TRUE ? 1 : 0);
  if (err != BISON_OK) throw_bison_exception(env, err);
}

JNIEXPORT void JNICALL Java_com_bdg_bison_Dynamic_nativeSetString(
    JNIEnv* env, jclass, jlong handle, jint name, jstring value) {
  jstring_view v(env, value);
  bison_error err = bison_set_string(from_jlong<bison_handle>(handle), name, v.c_str());
  if (err != BISON_OK) throw_bison_exception(env, err);
}

JNIEXPORT void JNICALL Java_com_bdg_bison_Dynamic_nativeSetKey(
    JNIEnv* env, jclass, jlong handle, jint name, jint value_hash) {
  bison_error err = bison_set_key(from_jlong<bison_handle>(handle), name, static_cast<bison_hash>(value_hash));
  if (err != BISON_OK) throw_bison_exception(env, err);
}

JNIEXPORT void JNICALL Java_com_bdg_bison_Dynamic_nativeSetObject(
    JNIEnv* env, jclass, jlong handle, jint name, jlong value_handle) {
  bison_error err =
      bison_set_object(from_jlong<bison_handle>(handle), name, from_jlong<bison_handle>(value_handle));
  if (err != BISON_OK) throw_bison_exception(env, err);
}

// ─── Scalar getters ─────────────────────────────────────────────────────

JNIEXPORT jint JNICALL Java_com_bdg_bison_Dynamic_nativeGetInt(JNIEnv* env, jclass, jlong handle, jint name) {
  int32_t out = 0;
  bison_error err = bison_get_int(from_jlong<bison_handle>(handle), name, &out);
  if (err != BISON_OK) throw_bison_exception(env, err);
  return out;
}

JNIEXPORT jfloat JNICALL Java_com_bdg_bison_Dynamic_nativeGetFloat(JNIEnv* env, jclass, jlong handle, jint name) {
  float out = 0;
  bison_error err = bison_get_float(from_jlong<bison_handle>(handle), name, &out);
  if (err != BISON_OK) throw_bison_exception(env, err);
  return out;
}

JNIEXPORT jboolean JNICALL Java_com_bdg_bison_Dynamic_nativeGetBool(JNIEnv* env, jclass, jlong handle, jint name) {
  int out = 0;
  bison_error err = bison_get_bool(from_jlong<bison_handle>(handle), name, &out);
  if (err != BISON_OK) throw_bison_exception(env, err);
  return out != 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL Java_com_bdg_bison_Dynamic_nativeGetString(JNIEnv* env, jclass, jlong handle, jint name) {
  bison_handle h = from_jlong<bison_handle>(handle);
  size_t len = 0;
  bison_error err = bison_get_string(h, name, nullptr, 0, &len);
  if (err != BISON_OK) {
    throw_bison_exception(env, err);
    return nullptr;
  }
  std::vector<char> buf(len + 1);
  err = bison_get_string(h, name, buf.data(), buf.size(), &len);
  if (err != BISON_OK) {
    throw_bison_exception(env, err);
    return nullptr;
  }
  return env->NewStringUTF(buf.data());
}

JNIEXPORT jint JNICALL Java_com_bdg_bison_Dynamic_nativeGetKey(JNIEnv* env, jclass, jlong handle, jint name) {
  bison_hash out = 0;
  bison_error err = bison_get_key(from_jlong<bison_handle>(handle), name, &out);
  if (err != BISON_OK) throw_bison_exception(env, err);
  return static_cast<jint>(out);
}

JNIEXPORT jlong JNICALL Java_com_bdg_bison_Dynamic_nativeGetObject(JNIEnv* env, jclass, jlong handle, jint name) {
  bison_handle out = nullptr;
  bison_error err = bison_get_object(from_jlong<bison_handle>(handle), name, &out);
  if (err != BISON_OK) {
    throw_bison_exception(env, err);
    return 0;
  }
  return to_jlong(out);
}

// ─── Vector fields ─────────────────────────────────────────────────────

JNIEXPORT void JNICALL Java_com_bdg_bison_Dynamic_nativeSetVectorBool(
    JNIEnv* env, jclass, jlong handle, jint name, jbooleanArray values) {
  jsize len = values ? env->GetArrayLength(values) : 0;
  std::vector<jboolean> src(len);
  std::vector<int> ints(len);
  if (len > 0) {
    env->GetBooleanArrayRegion(values, 0, len, src.data());
    for (jsize i = 0; i < len; ++i) ints[i] = src[i] != JNI_FALSE ? 1 : 0;
  }
  bison_error err =
      bison_set_vector_bool(from_jlong<bison_handle>(handle), name, ints.data(), static_cast<size_t>(len));
  if (err != BISON_OK) throw_bison_exception(env, err);
}

JNIEXPORT void JNICALL Java_com_bdg_bison_Dynamic_nativeSetVectorInt(
    JNIEnv* env, jclass, jlong handle, jint name, jintArray values) {
  jsize len = values ? env->GetArrayLength(values) : 0;
  std::vector<jint> buf(len);
  if (len > 0) env->GetIntArrayRegion(values, 0, len, buf.data());
  bison_error err = bison_set_vector_int(
      from_jlong<bison_handle>(handle), name, reinterpret_cast<const int32_t*>(buf.data()),
      static_cast<size_t>(len));
  if (err != BISON_OK) throw_bison_exception(env, err);
}

JNIEXPORT void JNICALL Java_com_bdg_bison_Dynamic_nativeSetVectorFloat(
    JNIEnv* env, jclass, jlong handle, jint name, jfloatArray values) {
  jsize len = values ? env->GetArrayLength(values) : 0;
  std::vector<jfloat> buf(len);
  if (len > 0) env->GetFloatArrayRegion(values, 0, len, buf.data());
  bison_error err =
      bison_set_vector_float(from_jlong<bison_handle>(handle), name, buf.data(), static_cast<size_t>(len));
  if (err != BISON_OK) throw_bison_exception(env, err);
}

JNIEXPORT void JNICALL Java_com_bdg_bison_Dynamic_nativeSetVectorBytes(
    JNIEnv* env, jclass, jlong handle, jint name, jbyteArray values) {
  jsize len = values ? env->GetArrayLength(values) : 0;
  std::vector<jbyte> buf(len);
  if (len > 0) env->GetByteArrayRegion(values, 0, len, buf.data());
  bison_error err = bison_set_vector_bytes(
      from_jlong<bison_handle>(handle), name, reinterpret_cast<const uint8_t*>(buf.data()),
      static_cast<size_t>(len));
  if (err != BISON_OK) throw_bison_exception(env, err);
}

JNIEXPORT jbooleanArray JNICALL Java_com_bdg_bison_Dynamic_nativeGetVectorBool(
    JNIEnv* env, jclass, jlong handle, jint name) {
  bool ok = false;
  std::vector<int> ints = read_vector<int>(from_jlong<bison_handle>(handle), name, bison_get_vector_bool, env, &ok);
  if (!ok) return nullptr;
  jbooleanArray out = env->NewBooleanArray(static_cast<jsize>(ints.size()));
  std::vector<jboolean> bools(ints.size());
  for (size_t i = 0; i < ints.size(); ++i) bools[i] = ints[i] != 0 ? JNI_TRUE : JNI_FALSE;
  if (!bools.empty()) env->SetBooleanArrayRegion(out, 0, static_cast<jsize>(bools.size()), bools.data());
  return out;
}

JNIEXPORT jintArray JNICALL Java_com_bdg_bison_Dynamic_nativeGetVectorInt(
    JNIEnv* env, jclass, jlong handle, jint name) {
  bool ok = false;
  std::vector<int32_t> vals =
      read_vector<int32_t>(from_jlong<bison_handle>(handle), name, bison_get_vector_int, env, &ok);
  if (!ok) return nullptr;
  jintArray out = env->NewIntArray(static_cast<jsize>(vals.size()));
  if (!vals.empty()) {
    env->SetIntArrayRegion(out, 0, static_cast<jsize>(vals.size()), reinterpret_cast<const jint*>(vals.data()));
  }
  return out;
}

JNIEXPORT jfloatArray JNICALL Java_com_bdg_bison_Dynamic_nativeGetVectorFloat(
    JNIEnv* env, jclass, jlong handle, jint name) {
  bool ok = false;
  std::vector<float> vals =
      read_vector<float>(from_jlong<bison_handle>(handle), name, bison_get_vector_float, env, &ok);
  if (!ok) return nullptr;
  jfloatArray out = env->NewFloatArray(static_cast<jsize>(vals.size()));
  if (!vals.empty()) env->SetFloatArrayRegion(out, 0, static_cast<jsize>(vals.size()), vals.data());
  return out;
}

JNIEXPORT jbyteArray JNICALL Java_com_bdg_bison_Dynamic_nativeGetVectorBytes(
    JNIEnv* env, jclass, jlong handle, jint name) {
  bool ok = false;
  std::vector<uint8_t> vals =
      read_vector<uint8_t>(from_jlong<bison_handle>(handle), name, bison_get_vector_bytes, env, &ok);
  if (!ok) return nullptr;
  jbyteArray out = env->NewByteArray(static_cast<jsize>(vals.size()));
  if (!vals.empty()) {
    env->SetByteArrayRegion(out, 0, static_cast<jsize>(vals.size()), reinterpret_cast<const jbyte*>(vals.data()));
  }
  return out;
}

// ─── Class registry ─────────────────────────────────────────────────────

JNIEXPORT void JNICALL Java_com_bdg_bison_Dynamic_nativeAddClass(
    JNIEnv* env, jclass, jint ns_hash, jlong proto_handle, jint parent_hash) {
  bison_error err = bison_add_class(
      static_cast<bison_hash>(ns_hash), from_jlong<bison_handle>(proto_handle),
      static_cast<bison_hash>(parent_hash), nullptr);
  if (err != BISON_OK) throw_bison_exception(env, err);
}

// ─── Methods ──────────────────────────────────────────────────────────────

JNIEXPORT void JNICALL Java_com_bdg_bison_Dynamic_nativeAddMethod(
    JNIEnv* env, jclass, jlong handle, jint name, jobject callback) {
  auto* ctx = new method_ctx{env->NewGlobalRef(callback)};
  bison_error err = bison_add_method(
      from_jlong<bison_handle>(handle), name, &method_trampoline, ctx, nullptr);
  if (err != BISON_OK) {
    env->DeleteGlobalRef(ctx->callback);
    delete ctx;
    throw_bison_exception(env, err);
  }
}

JNIEXPORT jlong JNICALL Java_com_bdg_bison_Dynamic_nativeCall(
    JNIEnv* env, jclass, jlong handle, jint name, jlong params_handle) {
  bison_handle result = nullptr;
  bison_error err = bison_call(
      from_jlong<bison_handle>(handle), name, from_jlong<bison_handle>(params_handle), &result);
  if (err != BISON_OK) {
    throw_bison_exception(env, err);
    return 0;
  }
  return to_jlong(result);
}

// ─── Serialization ──────────────────────────────────────────────────────

JNIEXPORT jlong JNICALL Java_com_bdg_bison_Dynamic_nativeSize(JNIEnv*, jclass, jlong handle) {
  return static_cast<jlong>(bison_size(from_jlong<bison_handle>(handle)));
}

JNIEXPORT jbyteArray JNICALL Java_com_bdg_bison_Dynamic_nativeSerialize(JNIEnv* env, jclass, jlong handle) {
  uint8_t* data = nullptr;
  size_t len = 0;
  bison_error err = bison_serialize(from_jlong<bison_handle>(handle), &data, &len);
  if (err != BISON_OK) {
    throw_bison_exception(env, err);
    return nullptr;
  }
  jbyteArray out = env->NewByteArray(static_cast<jsize>(len));
  if (len > 0) env->SetByteArrayRegion(out, 0, static_cast<jsize>(len), reinterpret_cast<const jbyte*>(data));
  bison_free_buffer(data);
  return out;
}

JNIEXPORT jstring JNICALL Java_com_bdg_bison_Dynamic_nativeToJson(JNIEnv* env, jclass, jlong handle, jint indent) {
  char* out = nullptr;
  bison_error err = bison_to_json(from_jlong<bison_handle>(handle), indent, &out);
  if (err != BISON_OK) {
    throw_bison_exception(env, err);
    return nullptr;
  }
  jstring result = env->NewStringUTF(out);
  bison_free_string(out);
  return result;
}

}  // extern "C"

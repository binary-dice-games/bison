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

namespace {

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

  jni_env_guard guard;
  JNIEnv* env = guard.env();
  if (!env) return;

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

  jclass attrs_local = env->FindClass("com/bdg/bison/Attributes");
  if (!attrs_local) return JNI_ERR;
  g_attributes_class = static_cast<jclass>(env->NewGlobalRef(attrs_local));
  g_attributes_ctor = env->GetMethodID(
      g_attributes_class, "<init>",
      "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;ZLjava/lang/String;Z)V");
  g_attributes_display_name = env->GetFieldID(g_attributes_class, "displayName", "Ljava/lang/String;");
  g_attributes_description = env->GetFieldID(g_attributes_class, "description", "Ljava/lang/String;");
  g_attributes_category = env->GetFieldID(g_attributes_class, "category", "Ljava/lang/String;");
  g_attributes_obsolete = env->GetFieldID(g_attributes_class, "obsolete", "Z");
  g_attributes_obsolete_message = env->GetFieldID(g_attributes_class, "obsoleteMessage", "Ljava/lang/String;");
  g_attributes_required = env->GetFieldID(g_attributes_class, "required", "Z");

  // Cached for rmi_jni.cpp's upcall trampolines (ProxyEvent.onEvent,
  // AuthHandler.authenticate) -- see jni_util.hpp's comment on why these
  // lookups must happen here rather than at first use.
  jclass proxy_event_local = env->FindClass("com/bdg/bison/rmi/ProxyEvent");
  if (!proxy_event_local) return JNI_ERR;
  g_proxy_event_class = static_cast<jclass>(env->NewGlobalRef(proxy_event_local));
  g_proxy_event_on_event = env->GetMethodID(g_proxy_event_class, "onEvent", "(Lcom/bdg/bison/Dynamic;)V");

  jclass auth_handler_local = env->FindClass("com/bdg/bison/rmi/AuthHandler");
  if (!auth_handler_local) return JNI_ERR;
  g_auth_handler_class = static_cast<jclass>(env->NewGlobalRef(auth_handler_local));
  g_auth_handler_authenticate =
      env->GetMethodID(g_auth_handler_class, "authenticate", "(Lcom/bdg/bison/Dynamic;)Ljava/lang/String;");

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

JNIEXPORT jlong JNICALL Java_com_bdg_bison_Dynamic_nativeFromYaml(JNIEnv* env, jclass, jstring yaml) {
  jstring_view v(env, yaml);
  bison_handle h = bison_from_yaml(v.c_str());
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

// ─── Indexed (numeric) field access ─────────────────────────────────────

JNIEXPORT void JNICALL Java_com_bdg_bison_Dynamic_nativeSetIntAt(
    JNIEnv* env, jclass, jlong handle, jlong index, jint value) {
  bison_error err = bison_set_int_at(from_jlong<bison_handle>(handle), static_cast<size_t>(index), value);
  if (err != BISON_OK) throw_bison_exception(env, err);
}

JNIEXPORT void JNICALL Java_com_bdg_bison_Dynamic_nativeSetFloatAt(
    JNIEnv* env, jclass, jlong handle, jlong index, jfloat value) {
  bison_error err = bison_set_float_at(from_jlong<bison_handle>(handle), static_cast<size_t>(index), value);
  if (err != BISON_OK) throw_bison_exception(env, err);
}

JNIEXPORT void JNICALL Java_com_bdg_bison_Dynamic_nativeSetBoolAt(
    JNIEnv* env, jclass, jlong handle, jlong index, jboolean value) {
  bison_error err = bison_set_bool_at(
      from_jlong<bison_handle>(handle), static_cast<size_t>(index), value == JNI_TRUE ? 1 : 0);
  if (err != BISON_OK) throw_bison_exception(env, err);
}

JNIEXPORT void JNICALL Java_com_bdg_bison_Dynamic_nativeSetStringAt(
    JNIEnv* env, jclass, jlong handle, jlong index, jstring value) {
  jstring_view v(env, value);
  bison_error err = bison_set_string_at(from_jlong<bison_handle>(handle), static_cast<size_t>(index), v.c_str());
  if (err != BISON_OK) throw_bison_exception(env, err);
}

JNIEXPORT void JNICALL Java_com_bdg_bison_Dynamic_nativeSetKeyAt(
    JNIEnv* env, jclass, jlong handle, jlong index, jint value_hash) {
  bison_error err = bison_set_key_at(
      from_jlong<bison_handle>(handle), static_cast<size_t>(index), static_cast<bison_hash>(value_hash));
  if (err != BISON_OK) throw_bison_exception(env, err);
}

JNIEXPORT void JNICALL Java_com_bdg_bison_Dynamic_nativeSetObjectAt(
    JNIEnv* env, jclass, jlong handle, jlong index, jlong value_handle) {
  bison_error err = bison_set_object_at(
      from_jlong<bison_handle>(handle), static_cast<size_t>(index), from_jlong<bison_handle>(value_handle));
  if (err != BISON_OK) throw_bison_exception(env, err);
}

JNIEXPORT jint JNICALL Java_com_bdg_bison_Dynamic_nativeGetIntAt(
    JNIEnv* env, jclass, jlong handle, jlong index) {
  int32_t out = 0;
  bison_error err = bison_get_int_at(from_jlong<bison_handle>(handle), static_cast<size_t>(index), &out);
  if (err != BISON_OK) throw_bison_exception(env, err);
  return out;
}

JNIEXPORT jfloat JNICALL Java_com_bdg_bison_Dynamic_nativeGetFloatAt(
    JNIEnv* env, jclass, jlong handle, jlong index) {
  float out = 0;
  bison_error err = bison_get_float_at(from_jlong<bison_handle>(handle), static_cast<size_t>(index), &out);
  if (err != BISON_OK) throw_bison_exception(env, err);
  return out;
}

JNIEXPORT jboolean JNICALL Java_com_bdg_bison_Dynamic_nativeGetBoolAt(
    JNIEnv* env, jclass, jlong handle, jlong index) {
  int out = 0;
  bison_error err = bison_get_bool_at(from_jlong<bison_handle>(handle), static_cast<size_t>(index), &out);
  if (err != BISON_OK) throw_bison_exception(env, err);
  return out != 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL Java_com_bdg_bison_Dynamic_nativeGetStringAt(
    JNIEnv* env, jclass, jlong handle, jlong index) {
  bison_handle h = from_jlong<bison_handle>(handle);
  size_t idx = static_cast<size_t>(index);
  size_t len = 0;
  bison_error err = bison_get_string_at(h, idx, nullptr, 0, &len);
  if (err != BISON_OK) {
    throw_bison_exception(env, err);
    return nullptr;
  }
  std::vector<char> buf(len + 1);
  err = bison_get_string_at(h, idx, buf.data(), buf.size(), &len);
  if (err != BISON_OK) {
    throw_bison_exception(env, err);
    return nullptr;
  }
  return env->NewStringUTF(buf.data());
}

JNIEXPORT jint JNICALL Java_com_bdg_bison_Dynamic_nativeGetKeyAt(
    JNIEnv* env, jclass, jlong handle, jlong index) {
  bison_hash out = 0;
  bison_error err = bison_get_key_at(from_jlong<bison_handle>(handle), static_cast<size_t>(index), &out);
  if (err != BISON_OK) throw_bison_exception(env, err);
  return static_cast<jint>(out);
}

JNIEXPORT jlong JNICALL Java_com_bdg_bison_Dynamic_nativeGetObjectAt(
    JNIEnv* env, jclass, jlong handle, jlong index) {
  bison_handle out = nullptr;
  bison_error err = bison_get_object_at(from_jlong<bison_handle>(handle), static_cast<size_t>(index), &out);
  if (err != BISON_OK) {
    throw_bison_exception(env, err);
    return 0;
  }
  return to_jlong(out);
}

// ─── Class registry ─────────────────────────────────────────────────────

JNIEXPORT void JNICALL Java_com_bdg_bison_Dynamic_nativeAddClass(
    JNIEnv* env, jclass, jint ns_hash, jlong proto_handle, jint parent_hash, jobject meta) {
  attributes_view attrs(env, meta);
  bison_error err = bison_add_class(
      static_cast<bison_hash>(ns_hash), from_jlong<bison_handle>(proto_handle),
      static_cast<bison_hash>(parent_hash), attrs.ptr());
  if (err != BISON_OK) throw_bison_exception(env, err);
}

JNIEXPORT jlong JNICALL Java_com_bdg_bison_Dynamic_nativeFindClass(
    JNIEnv*, jclass, jint ns_hash, jint klass_hash) {
  return to_jlong(bison_find_class(static_cast<bison_hash>(ns_hash), static_cast<bison_hash>(klass_hash)));
}

JNIEXPORT jlong JNICALL Java_com_bdg_bison_Dynamic_nativeInstantiate(
    JNIEnv*, jclass, jint ns_hash, jint klass_hash) {
  return to_jlong(bison_instantiate(static_cast<bison_hash>(ns_hash), static_cast<bison_hash>(klass_hash)));
}

JNIEXPORT void JNICALL Java_com_bdg_bison_Dynamic_nativeClearRegistry(JNIEnv*, jclass) {
  bison_clear_registry();
}

JNIEXPORT jobject JNICALL Java_com_bdg_bison_Dynamic_nativeGetClassAttributes(
    JNIEnv* env, jclass, jint ns_hash, jint klass_hash) {
  bison_attributes out{};
  bison_error err =
      bison_get_class_attributes(static_cast<bison_hash>(ns_hash), static_cast<bison_hash>(klass_hash), &out);
  if (err != BISON_OK) {
    throw_bison_exception(env, err);
    return nullptr;
  }
  return new_attributes(env, out);
}

JNIEXPORT jobject JNICALL Java_com_bdg_bison_Dynamic_nativeGetFieldAttributes(
    JNIEnv* env, jclass, jlong handle, jint field_hash) {
  bison_attributes out{};
  bison_error err = bison_get_field_attributes(from_jlong<bison_handle>(handle), field_hash, &out);
  if (err != BISON_OK) {
    throw_bison_exception(env, err);
    return nullptr;
  }
  return new_attributes(env, out);
}

JNIEXPORT jobject JNICALL Java_com_bdg_bison_Dynamic_nativeGetMethodAttributes(
    JNIEnv* env, jclass, jlong handle, jint method_hash) {
  bison_attributes out{};
  bison_error err = bison_get_method_attributes(from_jlong<bison_handle>(handle), method_hash, &out);
  if (err != BISON_OK) {
    throw_bison_exception(env, err);
    return nullptr;
  }
  return new_attributes(env, out);
}

// ─── Field registration with optional attribute metadata ───────────────

JNIEXPORT void JNICALL Java_com_bdg_bison_Dynamic_nativeAddFieldInt(
    JNIEnv* env, jclass, jlong handle, jint key, jint value, jobject meta) {
  attributes_view attrs(env, meta);
  bison_error err = bison_add_field_int(from_jlong<bison_handle>(handle), key, value, attrs.ptr());
  if (err != BISON_OK) throw_bison_exception(env, err);
}

JNIEXPORT void JNICALL Java_com_bdg_bison_Dynamic_nativeAddFieldFloat(
    JNIEnv* env, jclass, jlong handle, jint key, jfloat value, jobject meta) {
  attributes_view attrs(env, meta);
  bison_error err = bison_add_field_float(from_jlong<bison_handle>(handle), key, value, attrs.ptr());
  if (err != BISON_OK) throw_bison_exception(env, err);
}

JNIEXPORT void JNICALL Java_com_bdg_bison_Dynamic_nativeAddFieldBool(
    JNIEnv* env, jclass, jlong handle, jint key, jboolean value, jobject meta) {
  attributes_view attrs(env, meta);
  bison_error err =
      bison_add_field_bool(from_jlong<bison_handle>(handle), key, value == JNI_TRUE ? 1 : 0, attrs.ptr());
  if (err != BISON_OK) throw_bison_exception(env, err);
}

JNIEXPORT void JNICALL Java_com_bdg_bison_Dynamic_nativeAddFieldString(
    JNIEnv* env, jclass, jlong handle, jint key, jstring value, jobject meta) {
  jstring_view v(env, value);
  attributes_view attrs(env, meta);
  bison_error err = bison_add_field_string(from_jlong<bison_handle>(handle), key, v.c_str(), attrs.ptr());
  if (err != BISON_OK) throw_bison_exception(env, err);
}

JNIEXPORT void JNICALL Java_com_bdg_bison_Dynamic_nativeAddFieldKey(
    JNIEnv* env, jclass, jlong handle, jint key, jint value_hash, jobject meta) {
  attributes_view attrs(env, meta);
  bison_error err = bison_add_field_key(
      from_jlong<bison_handle>(handle), key, static_cast<bison_hash>(value_hash), attrs.ptr());
  if (err != BISON_OK) throw_bison_exception(env, err);
}

JNIEXPORT void JNICALL Java_com_bdg_bison_Dynamic_nativeAddFieldVectorBool(
    JNIEnv* env, jclass, jlong handle, jint key, jbooleanArray values, jobject meta) {
  jsize len = values ? env->GetArrayLength(values) : 0;
  std::vector<jboolean> src(len);
  std::vector<int> ints(len);
  if (len > 0) {
    env->GetBooleanArrayRegion(values, 0, len, src.data());
    for (jsize i = 0; i < len; ++i) ints[i] = src[i] != JNI_FALSE ? 1 : 0;
  }
  attributes_view attrs(env, meta);
  bison_error err = bison_add_field_vector_bool(
      from_jlong<bison_handle>(handle), key, ints.data(), static_cast<size_t>(len), attrs.ptr());
  if (err != BISON_OK) throw_bison_exception(env, err);
}

JNIEXPORT void JNICALL Java_com_bdg_bison_Dynamic_nativeAddFieldVectorInt(
    JNIEnv* env, jclass, jlong handle, jint key, jintArray values, jobject meta) {
  jsize len = values ? env->GetArrayLength(values) : 0;
  std::vector<jint> buf(len);
  if (len > 0) env->GetIntArrayRegion(values, 0, len, buf.data());
  attributes_view attrs(env, meta);
  bison_error err = bison_add_field_vector_int(
      from_jlong<bison_handle>(handle), key, reinterpret_cast<const int32_t*>(buf.data()), static_cast<size_t>(len),
      attrs.ptr());
  if (err != BISON_OK) throw_bison_exception(env, err);
}

JNIEXPORT void JNICALL Java_com_bdg_bison_Dynamic_nativeAddFieldVectorFloat(
    JNIEnv* env, jclass, jlong handle, jint key, jfloatArray values, jobject meta) {
  jsize len = values ? env->GetArrayLength(values) : 0;
  std::vector<jfloat> buf(len);
  if (len > 0) env->GetFloatArrayRegion(values, 0, len, buf.data());
  attributes_view attrs(env, meta);
  bison_error err = bison_add_field_vector_float(
      from_jlong<bison_handle>(handle), key, buf.data(), static_cast<size_t>(len), attrs.ptr());
  if (err != BISON_OK) throw_bison_exception(env, err);
}

JNIEXPORT void JNICALL Java_com_bdg_bison_Dynamic_nativeAddFieldVectorBytes(
    JNIEnv* env, jclass, jlong handle, jint key, jbyteArray values, jobject meta) {
  jsize len = values ? env->GetArrayLength(values) : 0;
  std::vector<jbyte> buf(len);
  if (len > 0) env->GetByteArrayRegion(values, 0, len, buf.data());
  attributes_view attrs(env, meta);
  bison_error err = bison_add_field_vector_bytes(
      from_jlong<bison_handle>(handle), key, reinterpret_cast<const uint8_t*>(buf.data()), static_cast<size_t>(len),
      attrs.ptr());
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

JNIEXPORT jstring JNICALL Java_com_bdg_bison_Dynamic_nativeToYaml(JNIEnv* env, jclass, jlong handle) {
  char* out = nullptr;
  bison_error err = bison_to_yaml(from_jlong<bison_handle>(handle), &out);
  if (err != BISON_OK) {
    throw_bison_exception(env, err);
    return nullptr;
  }
  jstring result = env->NewStringUTF(out);
  bison_free_string(out);
  return result;
}

}  // extern "C"

// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

/**
 * @file rmi_jni.cpp
 * @brief JNI glue for `com.bdg.bison.rmi.{Client,Proxy,Server,Future}`.
 *
 * Same shape as bison_jni.cpp, one layer over `rmi_c.h` instead of
 * `bison_c.h`. Only the standalone and TCP transports are bound (see
 * `Client`'s and `Server`'s doc comments for why); async operations
 * (`rmi_future_handle`), proxy events, and the server auth handler are all
 * exposed -- see docs/bindings.md for the binding's coverage.
 */

#include <jni.h>

#include <algorithm>
#include <cstring>

#include "jni_util.hpp"
#include "rmi_c.h"

using namespace bdg::bison::jni;

namespace {

/// Context captured for one `Proxy.onEvent()` subscription; leaked for the
/// process lifetime, the same tradeoff `bison_jni.cpp`'s `method_ctx` makes
/// for `Dynamic.addMethod()` -- `rmi_c.h` has no unsubscribe call either.
struct event_ctx {
  jobject callback; // global ref to the ProxyEvent instance
};

void JNICALL proxy_event_trampoline(bison_handle params, void* user) {
  auto* ctx = static_cast<event_ctx*>(user);

  jni_env_guard guard;
  JNIEnv* env = guard.env();
  if (!env) return;

  jobject j_params = env->CallStaticObjectMethod(g_dynamic_class, g_dynamic_wrap_borrowed, to_jlong(params));
  env->CallVoidMethod(ctx->callback, g_proxy_event_on_event, j_params);
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    log_error("rmi_jni", "ProxyEvent callback threw; the exception could not cross the C ABI and was dropped");
  }
  if (j_params) env->DeleteLocalRef(j_params);
}

/// Context captured for one `Server.listen()` call's auth handler; leaked
/// for the server handle's process lifetime -- it is fixed for as long as
/// the server keeps listening (see `rmi_server_listen`'s doc comment), so
/// there is no unregister call to hang a destructor off of either.
struct auth_ctx {
  jobject callback; // global ref to the AuthHandler instance
};

bool JNICALL auth_trampoline(bison_handle payload, char* identity_buf, size_t identity_buf_len, void* user) {
  auto* ctx = static_cast<auth_ctx*>(user);

  jni_env_guard guard;
  JNIEnv* env = guard.env();
  if (!env) return false;

  jobject j_payload = env->CallStaticObjectMethod(g_dynamic_class, g_dynamic_wrap_borrowed, to_jlong(payload));
  jstring identity = static_cast<jstring>(env->CallObjectMethod(ctx->callback, g_auth_handler_authenticate, j_payload));
  if (j_payload) env->DeleteLocalRef(j_payload);

  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    log_error("rmi_jni", "AuthHandler callback threw; the exception could not cross the C ABI and was dropped");
    return false;
  }

  if (!identity) return false;
  {
    // jstring_view must release its UTF-8 chars (its destructor, at the end
    // of this scope) before the local ref it reads them from is deleted --
    // ReleaseStringUTFChars on an already-deleted local ref aborts under
    // CheckJNI.
    jstring_view v(env, identity);
    if (identity_buf_len > 0) {
      size_t n = std::min(std::strlen(v.c_str()), identity_buf_len - 1);
      std::memcpy(identity_buf, v.c_str(), n);
      identity_buf[n] = '\0';
    }
  }
  env->DeleteLocalRef(identity);
  return true;
}

}  // namespace

extern "C" {

// ─── Client ───────────────────────────────────────────────────────────────

JNIEXPORT jlong JNICALL Java_com_bdg_bison_rmi_Client_nativeStandaloneCreate(JNIEnv*, jclass) {
  return to_jlong(rmi_standalone_create());
}

JNIEXPORT jlong JNICALL Java_com_bdg_bison_rmi_Client_nativeTcpCreate(
    JNIEnv* env, jclass, jstring host, jint port) {
  jstring_view h(env, host);
  return to_jlong(rmi_client_tcp_create(h.c_str(), static_cast<uint16_t>(port)));
}

JNIEXPORT void JNICALL Java_com_bdg_bison_rmi_Client_nativeConnect(
    JNIEnv* env, jclass, jlong handle, jlong params_handle) {
  rmi_error err = rmi_client_connect(from_jlong<rmi_client_handle>(handle), from_jlong<bison_handle>(params_handle));
  if (err != RMI_OK) throw_rmi_exception(env, err);
}

JNIEXPORT jlong JNICALL Java_com_bdg_bison_rmi_Client_nativeInstantiate(
    JNIEnv* env, jclass, jlong handle, jint ns_hash, jint class_hash, jlong params_handle) {
  rmi_proxy_handle proxy = nullptr;
  rmi_error err = rmi_client_instantiate(
      from_jlong<rmi_client_handle>(handle), static_cast<bison_hash>(ns_hash),
      static_cast<bison_hash>(class_hash), from_jlong<bison_handle>(params_handle), &proxy);
  if (err != RMI_OK) {
    throw_rmi_exception(env, err);
    return 0;
  }
  return to_jlong(proxy);
}

JNIEXPORT jlong JNICALL Java_com_bdg_bison_rmi_Client_nativeInstantiateAsync(
    JNIEnv* env, jclass, jlong handle, jint ns_hash, jint class_hash, jlong params_handle) {
  rmi_future_handle future = nullptr;
  rmi_error err = rmi_client_instantiate_async(
      from_jlong<rmi_client_handle>(handle), static_cast<bison_hash>(ns_hash),
      static_cast<bison_hash>(class_hash), from_jlong<bison_handle>(params_handle), &future);
  if (err != RMI_OK) {
    throw_rmi_exception(env, err);
    return 0;
  }
  return to_jlong(future);
}

JNIEXPORT void JNICALL Java_com_bdg_bison_rmi_Client_nativeDisconnect(JNIEnv* env, jclass, jlong handle) {
  rmi_error err = rmi_client_disconnect(from_jlong<rmi_client_handle>(handle));
  if (err != RMI_OK) throw_rmi_exception(env, err);
}

JNIEXPORT void JNICALL Java_com_bdg_bison_rmi_Client_nativeRelease(JNIEnv*, jclass, jlong handle) {
  rmi_client_release(from_jlong<rmi_client_handle>(handle));
}

// ─── Proxy ────────────────────────────────────────────────────────────────

JNIEXPORT void JNICALL Java_com_bdg_bison_rmi_Proxy_nativeSet(
    JNIEnv* env, jclass, jlong handle, jlong fields_handle, jlong timeout_ms) {
  rmi_error err = rmi_proxy_set(
      from_jlong<rmi_proxy_handle>(handle), from_jlong<bison_handle>(fields_handle), timeout_ms);
  if (err != RMI_OK) throw_rmi_exception(env, err);
}

JNIEXPORT jlong JNICALL Java_com_bdg_bison_rmi_Proxy_nativeSetAsync(
    JNIEnv* env, jclass, jlong handle, jlong fields_handle) {
  rmi_future_handle future = nullptr;
  rmi_error err =
      rmi_proxy_set_async(from_jlong<rmi_proxy_handle>(handle), from_jlong<bison_handle>(fields_handle), &future);
  if (err != RMI_OK) {
    throw_rmi_exception(env, err);
    return 0;
  }
  return to_jlong(future);
}

JNIEXPORT jlong JNICALL Java_com_bdg_bison_rmi_Proxy_nativeGet(
    JNIEnv* env, jclass, jlong handle, jlong projection_handle, jlong timeout_ms) {
  bison_handle result = nullptr;
  rmi_error err = rmi_proxy_get(
      from_jlong<rmi_proxy_handle>(handle), from_jlong<bison_handle>(projection_handle), &result, timeout_ms);
  if (err != RMI_OK) {
    throw_rmi_exception(env, err);
    return 0;
  }
  return to_jlong(result);
}

JNIEXPORT jlong JNICALL Java_com_bdg_bison_rmi_Proxy_nativeGetAsync(
    JNIEnv* env, jclass, jlong handle, jlong projection_handle) {
  rmi_future_handle future = nullptr;
  rmi_error err = rmi_proxy_get_async(
      from_jlong<rmi_proxy_handle>(handle), from_jlong<bison_handle>(projection_handle), &future);
  if (err != RMI_OK) {
    throw_rmi_exception(env, err);
    return 0;
  }
  return to_jlong(future);
}

JNIEXPORT void JNICALL Java_com_bdg_bison_rmi_Proxy_nativeClear(
    JNIEnv* env, jclass, jlong handle, jlong timeout_ms) {
  rmi_error err = rmi_proxy_clear(from_jlong<rmi_proxy_handle>(handle), timeout_ms);
  if (err != RMI_OK) throw_rmi_exception(env, err);
}

JNIEXPORT jlong JNICALL Java_com_bdg_bison_rmi_Proxy_nativeClearAsync(JNIEnv* env, jclass, jlong handle) {
  rmi_future_handle future = nullptr;
  rmi_error err = rmi_proxy_clear_async(from_jlong<rmi_proxy_handle>(handle), &future);
  if (err != RMI_OK) {
    throw_rmi_exception(env, err);
    return 0;
  }
  return to_jlong(future);
}

JNIEXPORT jlong JNICALL Java_com_bdg_bison_rmi_Proxy_nativeCall(
    JNIEnv* env, jclass, jlong handle, jint method_hash, jlong params_handle, jlong timeout_ms) {
  bison_handle result = nullptr;
  rmi_error err = rmi_proxy_call(
      from_jlong<rmi_proxy_handle>(handle), static_cast<bison_hash>(method_hash),
      from_jlong<bison_handle>(params_handle), &result, timeout_ms);
  if (err != RMI_OK) {
    throw_rmi_exception(env, err);
    return 0;
  }
  return to_jlong(result);
}

JNIEXPORT jlong JNICALL Java_com_bdg_bison_rmi_Proxy_nativeCallAsync(
    JNIEnv* env, jclass, jlong handle, jint method_hash, jlong params_handle) {
  rmi_future_handle future = nullptr;
  rmi_error err = rmi_proxy_call_async(
      from_jlong<rmi_proxy_handle>(handle), static_cast<bison_hash>(method_hash),
      from_jlong<bison_handle>(params_handle), &future);
  if (err != RMI_OK) {
    throw_rmi_exception(env, err);
    return 0;
  }
  return to_jlong(future);
}

JNIEXPORT void JNICALL Java_com_bdg_bison_rmi_Proxy_nativeOnEvent(
    JNIEnv* env, jclass, jlong handle, jint event_hash, jobject callback) {
  auto* ctx = new event_ctx{env->NewGlobalRef(callback)};
  rmi_error err = rmi_proxy_on_event(
      from_jlong<rmi_proxy_handle>(handle), static_cast<bison_hash>(event_hash), &proxy_event_trampoline, ctx);
  if (err != RMI_OK) {
    env->DeleteGlobalRef(ctx->callback);
    delete ctx;
    throw_rmi_exception(env, err);
  }
}

JNIEXPORT void JNICALL Java_com_bdg_bison_rmi_Proxy_nativeRelease(JNIEnv*, jclass, jlong handle) {
  rmi_proxy_release(from_jlong<rmi_proxy_handle>(handle));
}

// ─── Server ───────────────────────────────────────────────────────────────

JNIEXPORT jlong JNICALL Java_com_bdg_bison_rmi_Server_nativeTcpCreate(
    JNIEnv* env, jclass, jstring host, jint port) {
  jstring_view h(env, host);
  return to_jlong(rmi_server_tcp_create(h.c_str(), static_cast<uint16_t>(port)));
}

JNIEXPORT void JNICALL Java_com_bdg_bison_rmi_Server_nativeListen(
    JNIEnv* env, jclass, jlong handle, jlong params_handle, jobject auth_handler) {
  rmi_auth_fn auth_fn = nullptr;
  auth_ctx* ctx = nullptr;
  if (auth_handler) {
    ctx = new auth_ctx{env->NewGlobalRef(auth_handler)};
    auth_fn = &auth_trampoline;
  }
  rmi_error err = rmi_server_listen(
      from_jlong<rmi_server_handle>(handle), from_jlong<bison_handle>(params_handle), auth_fn, ctx);
  if (err != RMI_OK) {
    if (ctx) {
      env->DeleteGlobalRef(ctx->callback);
      delete ctx;
    }
    throw_rmi_exception(env, err);
  }
}

JNIEXPORT void JNICALL Java_com_bdg_bison_rmi_Server_nativeStop(JNIEnv*, jclass, jlong handle) {
  rmi_server_stop(from_jlong<rmi_server_handle>(handle));
}

JNIEXPORT void JNICALL Java_com_bdg_bison_rmi_Server_nativeRelease(JNIEnv*, jclass, jlong handle) {
  rmi_server_release(from_jlong<rmi_server_handle>(handle));
}

// ─── Future ───────────────────────────────────────────────────────────────

JNIEXPORT void JNICALL Java_com_bdg_bison_rmi_Future_nativeWait(
    JNIEnv* env, jclass, jlong handle, jlong timeout_ms) {
  rmi_error err = rmi_future_wait(from_jlong<rmi_future_handle>(handle), timeout_ms);
  if (err != RMI_OK) throw_rmi_exception(env, err);
}

JNIEXPORT jlong JNICALL Java_com_bdg_bison_rmi_Future_nativeGetDynamic(JNIEnv* env, jclass, jlong handle) {
  rmi_future_handle future = from_jlong<rmi_future_handle>(handle);
  bison_handle out = nullptr;
  rmi_error err = rmi_future_get_dynamic(&future, &out);
  if (err != RMI_OK) {
    throw_rmi_exception(env, err);
    return 0;
  }
  return to_jlong(out);
}

JNIEXPORT jlong JNICALL Java_com_bdg_bison_rmi_Future_nativeGetProxy(JNIEnv* env, jclass, jlong handle) {
  rmi_future_handle future = from_jlong<rmi_future_handle>(handle);
  rmi_proxy_handle out = nullptr;
  rmi_error err = rmi_future_get_proxy(&future, &out);
  if (err != RMI_OK) {
    throw_rmi_exception(env, err);
    return 0;
  }
  return to_jlong(out);
}

JNIEXPORT void JNICALL Java_com_bdg_bison_rmi_Future_nativeRelease(JNIEnv*, jclass, jlong handle) {
  rmi_future_release(from_jlong<rmi_future_handle>(handle));
}

}  // extern "C"

// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

/**
 * @file rmi_jni.cpp
 * @brief JNI glue for `com.bdg.bison.rmi.{Client,Proxy,Server}`.
 *
 * Same shape as bison_jni.cpp, one layer over `rmi_c.h` instead of
 * `bison_c.h`. Only the standalone and TCP transports are bound (see
 * `Client`'s and `Server`'s doc comments for why), and only the synchronous
 * (non-`_async`) proxy operations -- `rmi_future_handle` is not yet exposed
 * by this binding.
 */

#include <jni.h>

#include "jni_util.hpp"
#include "rmi_c.h"

using namespace bdg::bison::jni;

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

JNIEXPORT void JNICALL Java_com_bdg_bison_rmi_Proxy_nativeClear(
    JNIEnv* env, jclass, jlong handle, jlong timeout_ms) {
  rmi_error err = rmi_proxy_clear(from_jlong<rmi_proxy_handle>(handle), timeout_ms);
  if (err != RMI_OK) throw_rmi_exception(env, err);
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
    JNIEnv* env, jclass, jlong handle, jlong params_handle) {
  rmi_error err = rmi_server_listen(
      from_jlong<rmi_server_handle>(handle), from_jlong<bison_handle>(params_handle), nullptr, nullptr);
  if (err != RMI_OK) throw_rmi_exception(env, err);
}

JNIEXPORT void JNICALL Java_com_bdg_bison_rmi_Server_nativeStop(JNIEnv*, jclass, jlong handle) {
  rmi_server_stop(from_jlong<rmi_server_handle>(handle));
}

JNIEXPORT void JNICALL Java_com_bdg_bison_rmi_Server_nativeRelease(JNIEnv*, jclass, jlong handle) {
  rmi_server_release(from_jlong<rmi_server_handle>(handle));
}

}  // extern "C"

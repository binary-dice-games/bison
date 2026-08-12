// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

#include "jni_util.hpp"

#if defined(__ANDROID__)
#include <android/log.h>
#else
#include <cstdio>
#endif

namespace bdg::bison::jni {

void log_error(const char* tag, const char* message) {
#if defined(__ANDROID__)
  __android_log_print(ANDROID_LOG_ERROR, tag, "%s", message);
#else
  std::fprintf(stderr, "[%s] %s\n", tag, message);
#endif
}

namespace {

const char* bison_error_message(bison_error err) {
  switch (err) {
    case BISON_OK: return "ok";
    case BISON_ERR_NULL: return "a required handle or pointer argument was null";
    case BISON_ERR_TYPE: return "the field holds a different type than requested";
    case BISON_ERR_NOT_FOUND: return "method or field not found";
    case BISON_ERR_DUPLICATE: return "attempted to add a duplicate class or method";
    case BISON_ERR_EXCEPTION: return "an unexpected exception was caught in the native library";
    case BISON_ERR_PARSE: return "input failed to parse";
  }
  return "unknown bison error";
}

const char* rmi_error_message(rmi_error err) {
  switch (err) {
    case RMI_OK: return "ok";
    case RMI_ERR_NULL: return "a required handle or pointer argument was null";
    case RMI_ERR_INVALID_STATE: return "operation invalid for the current state (e.g. not connected)";
    case RMI_ERR_TIMEOUT: return "request timed out";
    case RMI_ERR_REMOTE_EXCEPTION: return "the server raised an exception";
    case RMI_ERR_TRANSPORT: return "transport error (network, connection, etc.)";
    case RMI_ERR_EXCEPTION: return "an unexpected exception was caught in the native library";
  }
  return "unknown rmi error";
}

// Throws `klass(int, String)` in @p env; if the constructor or class itself
// can't be found (binding jar/native library mismatch), falls back to a
// plain RuntimeException so the failure is still visible instead of being
// silently swallowed by a pending-exception JNI call further down the stack.
void throw_with_code(JNIEnv* env, const char* klass, int code, const char* message) {
  jclass cls = env->FindClass(klass);
  if (!cls) {
    env->ExceptionClear();
    env->ThrowNew(env->FindClass("java/lang/RuntimeException"), message);
    return;
  }
  jmethodID ctor = env->GetMethodID(cls, "<init>", "(ILjava/lang/String;)V");
  if (!ctor) {
    env->ExceptionClear();
    env->ThrowNew(env->FindClass("java/lang/RuntimeException"), message);
    return;
  }
  jstring jmessage = env->NewStringUTF(message);
  jthrowable ex = static_cast<jthrowable>(
      env->NewObject(cls, ctor, static_cast<jint>(code), jmessage));
  env->Throw(ex);
}

}  // namespace

void throw_bison_exception(JNIEnv* env, bison_error err) {
  throw_with_code(env, "com/bdg/bison/BisonException", static_cast<int>(err), bison_error_message(err));
}

void throw_rmi_exception(JNIEnv* env, rmi_error err) {
  throw_with_code(env, "com/bdg/bison/rmi/RmiException", static_cast<int>(err), rmi_error_message(err));
}

}  // namespace bdg::bison::jni

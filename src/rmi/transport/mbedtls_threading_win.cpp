// MIT License © 2025 Binary Dice Games
/**
 * @file mbedtls_threading_win.cpp
 * @brief Native Windows (MSVC/mingw64 outside MSYS2) mbedTLS mutex
 *        callbacks, backing MBEDTLS_THREADING_ALT (see
 *        cmake/mbedtls_user_config.h and mbedtls_threading.hpp).
 *
 * mbedtls_threading_mutex_t is CRITICAL_SECTION here (cmake/threading_alt.h),
 * so these callbacks are thin wrappers around the matching Win32 API.
 */
#include "src/rmi/transport/mbedtls_threading.hpp"

#include <windows.h>

#include <mbedtls/threading.h>

#include <mutex>

namespace bdg::bison::rmi::transport {

namespace {

void mutex_init(mbedtls_threading_mutex_t* mutex) {
  InitializeCriticalSection(mutex);
}

void mutex_free(mbedtls_threading_mutex_t* mutex) {
  DeleteCriticalSection(mutex);
}

int mutex_lock(mbedtls_threading_mutex_t* mutex) {
  EnterCriticalSection(mutex);
  return 0;
}

int mutex_unlock(mbedtls_threading_mutex_t* mutex) {
  LeaveCriticalSection(mutex);
  return 0;
}

} // namespace

void ensure_mbedtls_threading_initialized() {
  static std::once_flag once;
  std::call_once(once, [] { mbedtls_threading_set_alt(mutex_init, mutex_free, mutex_lock, mutex_unlock); });
}

} // namespace bdg::bison::rmi::transport

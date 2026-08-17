// MIT License © 2025 Binary Dice Games
/**
 * @file threading_alt.h
 * @brief `mbedtls_threading_mutex_t` definition for `MBEDTLS_THREADING_ALT`
 *        (see cmake/mbedtls_user_config.h). This exact filename is required:
 *        extern/mbedtls/include/mbedtls/threading.h itself does
 *        `#include "threading_alt.h"` when MBEDTLS_THREADING_ALT is defined
 *        (its own doc comment on that option specifies the name).
 *
 * Used only on genuinely native Windows (MSVC or mingw64 outside MSYS2),
 * where <pthread.h> -- which MBEDTLS_THREADING_PTHREAD requires -- is not
 * available. Linux and MSYS2 use MBEDTLS_THREADING_PTHREAD instead, which
 * defines this type itself.
 *
 * WIN32_LEAN_AND_MEAN: this header is dragged in by threading.h across
 * extern/mbedtls's own library sources, most of which aren't built with
 * that macro pre-defined (unlike bison's own target -- see CMakeLists.txt);
 * without it, <windows.h> pulls in the legacy <winsock.h>, which redefines
 * struct timeval/hostent/etc. and collides with <winsock2.h> (included by
 * mbedtls's net_sockets.c) wherever <windows.h> is transitively included
 * first.
 */
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef CRITICAL_SECTION mbedtls_threading_mutex_t;

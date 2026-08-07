// MIT License © 2025 Binary Dice Games
/**
 * @file mbedtls_user_config.h
 * @brief mbedTLS build-config override, wired in via CMake's
 *        MBEDTLS_USER_CONFIG_FILE (appended after extern/mbedtls's default
 *        mbedtls_config.h -- see extern/mbedtls's own doc comment on that
 *        option for the mechanism).
 *
 * Enables MBEDTLS_THREADING_C + MBEDTLS_THREADING_PTHREAD.
 *
 * bison's tls_socket_transport (src/rmi/transport/tls_socket_transport.cpp,
 * tls_stream_state.hpp) runs TLS handshakes concurrently on separate OS
 * threads by design: a client's open() handshakes synchronously on the
 * caller's thread while a server's accepted connections each handshake on
 * their own dedicated thread (src/rmi/DESIGN.md §13). mbedTLS's own docs
 * (mbedtls_config.h's MBEDTLS_USE_PSA_CRYPTO comment) are explicit that this
 * requires MBEDTLS_THREADING_C: "In multithreaded applications, you must
 * also enable MBEDTLS_THREADING_C, unless only one thread ever calls PSA
 * functions" -- without it, mbedTLS's process-wide shared state (the PSA
 * crypto key-slot table, lazily-initialized ciphersuite tables, etc.) has
 * no internal locking, which is a real thread-safety gap for any
 * application -- like this one -- that runs concurrent handshakes, not
 * merely a sanitizer false positive.
 *
 * Also disables MBEDTLS_SELF_TEST (on by default). It instruments several
 * library-internal functions (e.g. extern/mbedtls/library/ecp.c's
 * ecp_double_jac/ecp_add_mixed/mbedtls_mpi_mul_mod) with unsynchronized
 * file-static counters (add_count/dbl_count/mul_count) used only by that
 * library's own *_self_test() entry points -- which nothing in this
 * codebase calls. Concurrent handshakes on separate threads legitimately
 * touch these counters at the same time; since they're write-only outside
 * the unused self-test functions, the race has no effect on any
 * cryptographic operation's correctness, but disabling the dead
 * instrumentation entirely is cleaner than suppressing an open-ended set of
 * call sites that increment it.
 */
#pragma once

#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_PTHREAD

#undef MBEDTLS_SELF_TEST

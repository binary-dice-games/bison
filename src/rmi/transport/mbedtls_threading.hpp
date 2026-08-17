// MIT License © 2025 Binary Dice Games
/**
 * @file mbedtls_threading.hpp
 * @brief Installs mbedTLS's thread-safety mutex callbacks before any other
 *        mbedTLS function runs. Implemented per-platform
 *        (`mbedtls_threading_win.cpp`/`mbedtls_threading_posix.cpp`) since
 *        cmake/mbedtls_user_config.h selects a different mbedTLS threading
 *        backend per platform: MBEDTLS_THREADING_PTHREAD (Linux/MSYS2) wires
 *        itself up automatically, but MBEDTLS_THREADING_ALT (genuinely
 *        native Windows) requires the application to call
 *        mbedtls_threading_set_alt() itself.
 */
#pragma once

namespace bdg::bison::rmi::transport {

/**
 * @brief Ensures mbedTLS's threading callbacks are installed. Safe to call
 *        from multiple threads and more than once; only the first call has
 *        an effect. Must happen before any other mbedTLS function -- called
 *        from tls_stream_state's constructor (tls_stream_state.hpp).
 */
void ensure_mbedtls_threading_initialized();

} // namespace bdg::bison::rmi::transport

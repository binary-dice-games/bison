// MIT License © 2025 Binary Dice Games
/**
 * @file mbedtls_threading_posix.cpp
 * @brief Linux/MSYS2 mbedTLS threading init (see mbedtls_threading.hpp).
 *        A no-op: cmake/mbedtls_user_config.h selects
 *        MBEDTLS_THREADING_PTHREAD on these platforms, which wires up
 *        pthread-backed mutexes without any application call.
 */
#include "src/rmi/transport/mbedtls_threading.hpp"

namespace bdg::bison::rmi::transport {

void ensure_mbedtls_threading_initialized() {}

} // namespace bdg::bison::rmi::transport

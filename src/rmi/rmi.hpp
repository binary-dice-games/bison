// MIT License © 2025 Binary Dice Games
#pragma once

/**
 * @file rmi.hpp
 * @brief Master include for the Bison RMI framework.
 *
 * Include this single header to get the full client/server API, shared
 * protocol utilities, and the in-memory transport for unit testing.
 *
 * @code{.cpp}
 * #include "src/rmi/rmi.hpp"
 * using namespace bdg::bison::rmi;
 * @endcode
 */

#include "src/rmi/shared/constants.hpp"
#include "src/rmi/shared/ids.hpp"
#include "src/rmi/shared/envelope.hpp"
#include "src/rmi/transport/memory_transport.hpp"
#include "src/rmi/server/context.hpp"
#include "src/rmi/client/remote_dynamic.hpp"
#include "src/rmi/client/client.hpp"
#include "src/rmi/server/server.hpp"

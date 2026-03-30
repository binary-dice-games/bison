// MIT License © 2025 Binary Dice Games
/**
 * @file rmi.hpp
 * @brief Umbrella include for the Bison RMI framework.
 *
 * Include this header to access the full remote-method-invocation stack:
 * envelope constants/helpers, ID generation, in-memory transport,
 * client/server runtime, and remote object proxy types.
 */
#pragma once

#include "src/rmi/client/client.hpp"
#include "src/rmi/client/proxy_dynamic.hpp"
#include "src/rmi/server/context.hpp"
#include "src/rmi/server/server.hpp"
#include "src/rmi/shared/constants.hpp"
#include "src/rmi/shared/envelope.hpp"
#include "src/rmi/shared/ids.hpp"
#include "src/rmi/shared/schemas.hpp"
#include "src/rmi/transport/memory_transport.hpp"

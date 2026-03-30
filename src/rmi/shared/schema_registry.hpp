// MIT License © 2025 Binary Dice Games
/**
 * @file schema_registry.hpp
 * @brief Global self-registration helpers for RMI schemas.
 */
#pragma once

#include <concepts>

namespace bdg::bison::rmi::shared::schema_registry {

using schema_registration_fn = void (*)();

/**
 * @brief Add a schema registration callback to the global registry.
 * @param fn Callback that performs one schema registration.
 */
void add_schema_registration(schema_registration_fn fn);

/**
 * @brief Execute all registered schema callbacks.
 */
void register_all_schemas();

/**
 * @brief Concept for types that expose a static schema registration entry
 * point.
 */
template <typename T>
concept schema_provider = requires {
  { T::register_schemas() } -> std::same_as<void>;
};

/**
 * @brief Register a schema provider type in the global callback collection.
 * @tparam T Type that implements `static void register_schemas()`.
 * @return Always `true`, suitable for static initialization tokens.
 */
template <schema_provider T>
bool register_provider() {
  add_schema_registration(&T::register_schemas);
  return true;
}

} // namespace bdg::bison::rmi::shared::schema_registry

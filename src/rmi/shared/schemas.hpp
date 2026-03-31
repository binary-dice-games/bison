// MIT License © 2025 Binary Dice Games
/**
 * @file schemas.hpp
 * @brief Central schema map and registration helpers for RMI.
 */
#pragma once

#include "src/rmi/shared/constants.hpp"

#include <unordered_map>

namespace bdg::bison::rmi::shared {

using schema_map = std::
    unordered_map<bison::key_t, bison::dynamic, bison::key_t, bison::key_t>;

inline const schema_map& get_schemas() {
  using namespace constants;
  using namespace bdg::bison;

  static const schema_map schemas = {
      {
          CLASS_ERROR,
          dynamic{
              CLASS_ERROR,
              {
                  {FIELD_ERROR_CODE, field{key_t{0u}}},
                  {FIELD_ERROR_MESSAGE, field{std::string{}}},
                  {FIELD_ERROR_DETAILS, field{dynamic_ptr{}}},
              },
          },
      },
      {
          CLASS_ENVELOPE,
          dynamic{
              CLASS_ENVELOPE,
              {
                  {FIELD_VERSION, field{int32_t{1}}},
                  {FIELD_KIND, field{key_t{0u}}},
                  {FIELD_OP, field{key_t{0u}}},
                  {FIELD_REQUEST_ID, field{key_t{0u}}},
                  {FIELD_OBJECT_ID, field{key_t{0u}}},
                  {FIELD_PAYLOAD, field{buffer{}}},
                  {FIELD_ERROR, field{buffer{}}},
                  {FIELD_WITH_SCHEMA, field{false}},
                  {FIELD_ONEWAY, field{false}},
              },
          },
      },
  };

  return schemas;
}

inline void register_all_schemas() {
  using namespace bdg::bison;

  for (const auto& [schema_id, schema] : get_schemas()) {
    dynamic::addClass(0U, dynamic_ptr{schema.clone()});
  }
}

} // namespace bdg::bison::rmi::shared

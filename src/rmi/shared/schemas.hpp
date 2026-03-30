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
  static const schema_map schemas = {
      {
          constants::CLASS_ERROR,
          bison::dynamic{
              constants::CLASS_ERROR,
              {
                  {constants::FIELD_ERROR_CODE, bison::field{bison::key_t{0u}}},
                  {constants::FIELD_ERROR_MESSAGE, bison::field{std::string{}}},
                  {constants::FIELD_ERROR_DETAILS, bison::field{bison::dynamic_ptr{}}},
              },
          },
      },
      {
          constants::CLASS_ENVELOPE,
          bison::dynamic{
              constants::CLASS_ENVELOPE,
              {
                  {constants::FIELD_VERSION, bison::field{int32_t{1}}},
                  {constants::FIELD_KIND, bison::field{bison::key_t{0u}}},
                  {constants::FIELD_OP, bison::field{bison::key_t{0u}}},
                  {constants::FIELD_REQUEST_ID, bison::field{bison::key_t{0u}}},
                  {constants::FIELD_OBJECT_ID, bison::field{bison::key_t{0u}}},
                  {constants::FIELD_PAYLOAD, bison::field{bison::buffer{}}},
                  {constants::FIELD_ERROR, bison::field{bison::buffer{}}},
                  {constants::FIELD_WITH_SCHEMA, bison::field{false}},
                  {constants::FIELD_ONEWAY, bison::field{false}},
              },
          },
      },
  };

  return schemas;
}

inline void register_all_schemas() {
  for (const auto& [schema_id, schema] : get_schemas()) {
    bison::dynamic::addClass(0U, bison::dynamic_ptr{schema.clone()});
  }
}

} // namespace bdg::bison::rmi::shared

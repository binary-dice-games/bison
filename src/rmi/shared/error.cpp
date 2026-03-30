// MIT License © 2025 Binary Dice Games
/**
 * @file error.cpp
 * @brief Implementation of the RMI protocol error codec.
 */
#include "src/rmi/shared/error.hpp"

#include "src/rmi/shared/constants.hpp"
#include "src/rmi/shared/payload.hpp"

namespace bdg::bison::rmi::shared {

error::error(bison::key_t code_arg, std::string message)
    : std::runtime_error(std::move(message)), code(code_arg) {}

bison::buffer error::encode() const {
  using namespace constants;
  bison::dynamic err;
  err[FIELD_ERROR_CODE] = code;
  err[FIELD_ERROR_MESSAGE] = std::string{what()};
  return payload{std::move(err)}.encode();
}

error error::decode(const bison::buffer& bytes) {
  using namespace constants;
  payload decoded = payload::decode(bytes);
  return error{
      decoded.value.as<bison::key_t>(FIELD_ERROR_CODE),
      decoded.value.as<std::string>(FIELD_ERROR_MESSAGE)};
}

} // namespace bdg::bison::rmi::shared

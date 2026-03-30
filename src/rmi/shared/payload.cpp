// MIT License © 2025 Binary Dice Games
/**
 * @file payload.cpp
 * @brief Implementation of the serialized dynamic payload codec.
 */
#include "src/rmi/shared/payload.hpp"

namespace bdg::bison::rmi::shared {

payload::payload(bison::dynamic val) : value(std::move(val)) {}

bison::buffer payload::encode() const {
  bison::buffer_serializer out;
  value.serialize(out);
  return out.release();
}

payload payload::decode(const bison::buffer& bytes) {
  if (bytes.empty()) {
    return payload{};
  }
  bison::buffer_deserializer in(bytes);
  auto dyn = bison::dynamic::deserialize(in);
  return payload{std::move(*dyn)};
}

} // namespace bdg::bison::rmi::shared

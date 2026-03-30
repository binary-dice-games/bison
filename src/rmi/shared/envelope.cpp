// MIT License © 2025 Binary Dice Games
/**
 * @file envelope.cpp
 * @brief Envelope encoding/decoding implementation for the RMI wire protocol.
 */
#include "src/rmi/shared/envelope.hpp"

#include <shared_mutex>

namespace bdg::bison::rmi::shared {

envelope::envelope(
    bison::key_t kind_arg,
    bison::key_t op_arg,
    bison::key_t request_id_arg,
    bison::key_t object_id_arg,
    bool oneway_arg,
    ::bdg::bison::rmi::shared::payload&& pl)
    : version(constants::PROTOCOL_VERSION),
      kind(kind_arg),
      op(op_arg),
      request_id(request_id_arg),
      object_id(object_id_arg),
      oneway(oneway_arg),
      payload(pl.encode()),
      error{} {}

envelope::envelope(
    bison::key_t kind_arg,
    bison::key_t op_arg,
    bison::key_t request_id_arg,
    bison::key_t object_id_arg,
    bool oneway_arg,
    ::bdg::bison::rmi::shared::payload&& pl,
    ::bdg::bison::rmi::shared::error&& err)
    : version(constants::PROTOCOL_VERSION),
      kind(kind_arg),
      op(op_arg),
      request_id(request_id_arg),
      object_id(object_id_arg),
      oneway(oneway_arg),
      payload(pl.encode()),
      error(err.encode()) {}

bison::buffer envelope::encode() const {
  using namespace constants;
  bison::dynamic env{CLASS_ENVELOPE};
  env[FIELD_VERSION] = version;
  env[FIELD_KIND] = kind;
  env[FIELD_OP] = op;
  env[FIELD_REQUEST_ID] = request_id;
  env[FIELD_OBJECT_ID] = object_id;
  env[FIELD_ONEWAY] = oneway;
  env[FIELD_PAYLOAD] = payload;
  env[FIELD_ERROR] = error;
  bison::buffer_serializer out;
  env.serializeWithTemplate(out);
  return out.release();
}

envelope envelope::decode(const bison::buffer& bytes) {
  using namespace constants;
  bison::buffer_deserializer in(bytes);
  auto out = bison::dynamic::deserializeWithTemplate(in);

  ::bdg::bison::rmi::shared::envelope decoded;
  decoded.version = out->as<int32_t>(FIELD_VERSION);
  decoded.kind = out->as<bison::key_t>(FIELD_KIND);
  decoded.op = out->as<bison::key_t>(FIELD_OP);
  decoded.request_id = out->as<bison::key_t>(FIELD_REQUEST_ID);
  decoded.object_id = out->as<bison::key_t>(FIELD_OBJECT_ID);
  decoded.oneway = out->as<bool>(FIELD_ONEWAY);
  decoded.payload = out->as<bison::buffer>(FIELD_PAYLOAD);
  decoded.error = out->as<bison::buffer>(FIELD_ERROR);
  return decoded;
}

void envelope::register_schema() {
  using namespace constants;
  {
    std::shared_lock<std::shared_mutex> lk(bison::dynamic::getMutex());
    if (bison::dynamic::getClasses().count(CLASS_ENVELOPE))
      return;
  }
  auto proto = bison::dynamic_ptr{
      CLASS_ENVELOPE,
      {
          {FIELD_VERSION, bison::field{int32_t{1}}},
          {FIELD_KIND, bison::field{bison::key_t{0u}}},
          {FIELD_OP, bison::field{bison::key_t{0u}}},
          {FIELD_REQUEST_ID, bison::field{bison::key_t{0u}}},
          {FIELD_OBJECT_ID, bison::field{bison::key_t{0u}}},
          {FIELD_ONEWAY, bison::field{false}},
          {FIELD_PAYLOAD, bison::field{bison::buffer{}}},
          {FIELD_ERROR, bison::field{bison::buffer{}}},
      }};
  bison::dynamic::addClass(0U, proto);
}

} // namespace bdg::bison::rmi::shared

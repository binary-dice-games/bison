// MIT License © 2025 Binary Dice Games
/**
 * @file envelope.cpp
 * @brief Envelope encoding/decoding implementation for the RMI wire protocol.
 */
#include "src/rmi/shared/envelope.hpp"

#include "src/rmi/shared/schemas.hpp"

namespace bdg::bison::rmi::shared {

namespace {

bison::dynamic normalize_error_payload(const bison::dynamic& error) {
  bison::dynamic normalized{constants::CLASS_ERROR};
  error.forEach([&normalized](bison::key_t key, const bison::field& value) {
    if (key != bison::dynamic::CLASS) {
      normalized[key] = value;
    }
  });
  return normalized;
}

} // namespace

bison::buffer envelope::encode() const {
  using namespace constants;
  bison::buffer_serializer buffer;
  bison::buffer_serializer error_buffer;
  if (with_schema) {
    payload.serializeWithSchema(buffer);
  } else {
    payload.serialize(buffer);
  }

  normalize_error_payload(error).serializeWithSchema(error_buffer);

  bison::dynamic env{CLASS_ENVELOPE};
  env[FIELD_VERSION] = version;
  env[FIELD_KIND] = kind;
  env[FIELD_OP] = op;
  env[FIELD_REQUEST_ID] = request_id;
  env[FIELD_OBJECT_ID] = object_id;
  env[FIELD_WITH_SCHEMA] = with_schema;
  env[FIELD_PAYLOAD] = buffer.release();
  env[FIELD_ERROR] = error_buffer.release();
  env[FIELD_ONEWAY] = oneway;

  bison::buffer_serializer out;
  env.serializeWithSchema(out);
  return out.release();
}

envelope envelope::decode(const bison::buffer& bytes) {
  using namespace constants;
  bison::buffer_deserializer in(bytes);
  auto out = bison::dynamic::deserializeWithSchema(in);

  ::bdg::bison::rmi::shared::envelope decoded;
  decoded.version = out[FIELD_VERSION];
  decoded.kind = out[FIELD_KIND];
  decoded.op = out[FIELD_OP];
  decoded.request_id = out[FIELD_REQUEST_ID];
  decoded.object_id = out[FIELD_OBJECT_ID];
  decoded.with_schema = out[FIELD_WITH_SCHEMA];
  decoded.oneway = out[FIELD_ONEWAY];

  bison::buffer_deserializer buffer(out[FIELD_PAYLOAD].as<bison::buffer>());
  bison::buffer_deserializer error_buffer(out[FIELD_ERROR].as<bison::buffer>());
  if (decoded.with_schema) {
    decoded.payload = bison::dynamic::deserializeWithSchema(buffer);
  } else {
    decoded.payload = bison::dynamic::deserialize(buffer);
  }
  decoded.error = bison::dynamic::deserializeWithSchema(error_buffer);

  return decoded;
}

} // namespace bdg::bison::rmi::shared

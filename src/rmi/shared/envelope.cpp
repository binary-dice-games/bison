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
  if (with_schema) {
    payload.serializeWithSchema(buffer);
  } else {
    payload.serialize(buffer);
  }

  bison::dynamic env{CLASS_ENVELOPE};
  env[FIELD_VERSION] = version;
  env[FIELD_KIND] = kind;
  env[FIELD_OP] = op;
  env[FIELD_REQUEST_ID] = request_id;
  env[FIELD_OBJECT_ID] = object_id;
  env[FIELD_GROUP] = group;
  env[FIELD_WITH_SCHEMA] = with_schema;
  env[FIELD_PAYLOAD] = buffer.release();
  // Success responses (the overwhelming common case) never populate `error`
  // beyond its always-present CLASS field, so `findField(FIELD_ERROR_CODE)`
  // is null. Leave FIELD_ERROR unset in that case -- serializeWithSchema
  // falls back to the "__envelope" prototype's default (an empty buffer),
  // which is both cheaper (skips building+serializing a whole "__error"
  // object under the registry read lock) and smaller on the wire than
  // encoding an all-default "__error" object. `envelope::decode()` mirrors
  // this: an empty FIELD_ERROR buffer decodes back to a default-constructed
  // `error`, and `FIELD_ERROR_CODE` reads as 0 either way.
  if (error.findField(FIELD_ERROR_CODE) != nullptr) {
    bison::buffer_serializer error_buffer;
    normalize_error_payload(error).serializeWithSchema(error_buffer);
    env[FIELD_ERROR] = error_buffer.release();
  }
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
  decoded.group = out[FIELD_GROUP];
  decoded.with_schema = out[FIELD_WITH_SCHEMA];
  decoded.oneway = out[FIELD_ONEWAY];

  bison::buffer_deserializer buffer(out[FIELD_PAYLOAD].as<bison::buffer>());
  if (decoded.with_schema) {
    decoded.payload = bison::dynamic::deserializeWithSchema(buffer);
  } else {
    decoded.payload = bison::dynamic::deserialize(buffer);
  }

  // See envelope::encode()'s comment: an empty FIELD_ERROR buffer means "no
  // error" -- skip the deserializeWithSchema call and registry lock entirely
  // and leave `decoded.error` default-constructed (FIELD_ERROR_CODE reads as
  // 0 from it either way).
  if (auto error_bytes = out[FIELD_ERROR].as<bison::buffer>(); !error_bytes.empty()) {
    bison::buffer_deserializer error_buffer(error_bytes);
    decoded.error = bison::dynamic::deserializeWithSchema(error_buffer);
  }

  return decoded;
}

} // namespace bdg::bison::rmi::shared

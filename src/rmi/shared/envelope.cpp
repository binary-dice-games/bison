// MIT License © 2025 Binary Dice Games
/**
 * @file envelope.cpp
 * @brief Envelope encoding/decoding implementation for the RMI wire protocol.
 */
#include "src/rmi/shared/envelope.hpp"

#include <shared_mutex>

namespace bdg::bison::rmi::shared {

// rmi_error
/** @copydoc bdg::bison::rmi::shared::rmi_error::rmi_error */
rmi_error::rmi_error(bison::key_t code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

/** @copydoc bdg::bison::rmi::shared::rmi_error::code */
bison::key_t rmi_error::code() const { return code_; }

// register_envelope
/** @copydoc bdg::bison::rmi::shared::register_envelope */
void register_envelope() {
  using namespace constants;
  {
    std::shared_lock<std::shared_mutex> lk(bison::dynamic::getMutex());
    if (bison::dynamic::getClasses().count(CLASS_ENVELOPE)) return;
  }
  auto proto = bison::dynamic_ptr{
      CLASS_ENVELOPE,
      {
          {FIELD_VERSION,    bison::field{int32_t{1}}},
          {FIELD_KIND,       bison::field{bison::key_t{0u}}},
          {FIELD_OP,         bison::field{bison::key_t{0u}}},
          {FIELD_REQUEST_ID, bison::field{std::string{}}},
          {FIELD_OBJECT_ID,  bison::field{std::string{}}},
          {FIELD_ONEWAY,     bison::field{false}},
          {FIELD_PAYLOAD,    bison::field{std::string{}}},
          {FIELD_ERROR,      bison::field{std::string{}}},
      }};
  bison::dynamic::addClass(0U, proto);
}

// encode_payload / decode_payload
/** @copydoc bdg::bison::rmi::shared::encode_payload */
std::string encode_payload(const bison::dynamic& payload) {
  bison::buffer_serializer out;
  payload.serialize(out);
  auto bytes = out.release();
  return {bytes.begin(), bytes.end()};
}

/** @copydoc bdg::bison::rmi::shared::decode_payload */
std::shared_ptr<bison::dynamic> decode_payload(const std::string& bytes) {
  if (bytes.empty()) return std::make_shared<bison::dynamic>();
  bison::buffer_deserializer in(bytes);
  return bison::dynamic::deserialize(in);
}

/** @copydoc bdg::bison::rmi::shared::encode_error */
std::string encode_error(bison::key_t code, const std::string& message) {
  using namespace constants;
  bison::dynamic err;
  err[FIELD_ERROR_CODE]    = code;
  err[FIELD_ERROR_MESSAGE] = message;
  return encode_payload(err);
}

// encode_envelope / decode_envelope
/** @copydoc bdg::bison::rmi::shared::encode_envelope */
std::vector<char> encode_envelope(
    bison::key_t kind,
    bison::key_t op,
    const std::string& request_id,
    const std::string& object_id,
    bool oneway,
    const std::string& payload,
    const std::string& error) {
  using namespace constants;
  bison::dynamic env{CLASS_ENVELOPE};
  env[FIELD_VERSION]    = int32_t{PROTOCOL_VERSION};
  env[FIELD_KIND]       = kind;
  env[FIELD_OP]         = op;
  env[FIELD_REQUEST_ID] = request_id;
  env[FIELD_OBJECT_ID]  = object_id;
  env[FIELD_ONEWAY]     = oneway;
  env[FIELD_PAYLOAD]    = payload;
  env[FIELD_ERROR]      = error;
  bison::buffer_serializer out;
  env.serializeWithTemplate(out);
  return out.release();
}

/** @copydoc bdg::bison::rmi::shared::decode_envelope */
std::shared_ptr<bison::dynamic> decode_envelope(const std::vector<char>& bytes) {
  bison::buffer_deserializer in(bytes);
  return bison::dynamic::deserializeWithTemplate(in);
}

} // namespace bdg::bison::rmi::shared

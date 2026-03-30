// MIT License © 2025 Binary Dice Games
#pragma once

#include "src/core/bison.hpp"
#include "src/rmi/shared/constants.hpp"

#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace bdg::bison::rmi::shared {

/**
 * @brief RMI protocol error thrown on the client when the server returns an
 *        error response.
 */
class rmi_error : public std::runtime_error {
 public:
  rmi_error(bison::key_t code, std::string message)
      : std::runtime_error(std::move(message)), code_(code) {}

  bison::key_t code() const { return code_; }

 private:
  bison::key_t code_;
};

// ─── Envelope registration ────────────────────────────────────────────────────

/**
 * @brief Register the fixed envelope class prototype in the global Bison class
 *        registry.  Safe to call multiple times; subsequent calls are no-ops.
 *
 * Must be called before any protocol messages are serialised or deserialised.
 */
inline void register_envelope() {
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

// ─── Payload helpers ──────────────────────────────────────────────────────────

/**
 * @brief Serialise a `bison::dynamic` payload to a raw byte string using
 *        self-describing serialisation.
 */
inline std::string encode_payload(const bison::dynamic& payload) {
  bison::buffer_serializer out;
  payload.serialize(out);
  auto bytes = out.release();
  return {bytes.begin(), bytes.end()};
}

/**
 * @brief Deserialise a payload byte string back to a `bison::dynamic`.
 *        Returns an empty (anonymous) dynamic if @p bytes is empty.
 */
inline std::shared_ptr<bison::dynamic> decode_payload(const std::string& bytes) {
  if (bytes.empty()) return std::make_shared<bison::dynamic>();
  bison::buffer_deserializer in(bytes);
  return bison::dynamic::deserialize(in);
}

/**
 * @brief Serialise a canonical error object to a byte string.
 */
inline std::string encode_error(bison::key_t code, const std::string& message) {
  using namespace constants;
  bison::dynamic err;
  err[FIELD_ERROR_CODE]    = code;
  err[FIELD_ERROR_MESSAGE] = message;
  return encode_payload(err);
}

// ─── Envelope encode / decode ─────────────────────────────────────────────────

/**
 * @brief Build and template-serialise a protocol envelope frame.
 *
 * @param kind        Message kind token (KIND_REQUEST / RESPONSE / EVENT).
 * @param op          Operation token (OP_*).
 * @param request_id  Correlation identifier string.
 * @param object_id   Remote object identifier string (empty when N/A).
 * @param oneway      True when no response is expected.
 * @param payload     Self-describing serialised payload bytes.
 * @param error       Self-describing serialised error bytes (empty = no error).
 * @return Serialised envelope as a byte vector.
 */
inline std::vector<char> encode_envelope(
    bison::key_t      kind,
    bison::key_t      op,
    const std::string& request_id,
    const std::string& object_id,
    bool               oneway,
    const std::string& payload,
    const std::string& error = {}) {
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

/**
 * @brief Template-deserialise a protocol envelope frame.
 *
 * @param bytes  Raw frame bytes produced by `encode_envelope`.
 * @return Shared pointer to the reconstructed envelope `dynamic`.
 * @throws std::runtime_error on malformed data.
 */
inline std::shared_ptr<bison::dynamic> decode_envelope(
    const std::vector<char>& bytes) {
  bison::buffer_deserializer in(bytes);
  return bison::dynamic::deserializeWithTemplate(in);
}

} // namespace bdg::bison::rmi::shared

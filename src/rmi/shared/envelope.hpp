// MIT License © 2025 Binary Dice Games
/**
 * @file envelope.hpp
 * @brief Helpers for encoding/decoding RMI envelopes, payloads, and errors.
 */
#pragma once

#include "src/core/bison.hpp"
#include "src/rmi/shared/constants.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace bdg::bison::rmi::shared {

/**
 * @brief Protocol-level exception containing an RMI error code.
 */
class rmi_error : public std::runtime_error {
 public:
  /**
   * @brief Construct an RMI error.
   * @param code Canonical error code token.
   * @param message Human-readable error message.
   */
  rmi_error(bison::key_t code, std::string message);

  /** @brief Return the protocol error code token. */
  bison::key_t code() const;
 private:
  bison::key_t code_;
};

/** @brief Register the envelope class schema in the global class registry. */
void register_envelope();

/**
 * @brief Serialize a payload object to bytes.
 * @param payload Payload dynamic object.
 * @return Binary payload representation.
 */
std::string encode_payload(const bison::dynamic& payload);

/**
 * @brief Deserialize payload bytes into a dynamic object.
 * @param bytes Binary payload bytes.
 * @return Shared pointer to decoded payload object.
 */
std::shared_ptr<bison::dynamic> decode_payload(const std::string& bytes);

/**
 * @brief Build a serialized error object.
 * @param code Canonical error code token.
 * @param message Human-readable error message.
 * @return Serialized error bytes.
 */
std::string encode_error(bison::key_t code, const std::string& message);

/**
 * @brief Encode an RMI envelope into a transport frame.
 * @param kind Envelope kind token (`request`, `response`, `event`).
 * @param op Operation token.
 * @param request_id Correlation ID for request/response pairing.
 * @param object_id Target object ID (if applicable).
 * @param oneway Whether a response is expected.
 * @param payload Serialized operation payload bytes.
 * @param error Optional serialized error payload.
 * @return Encoded binary frame bytes.
 */
std::vector<char> encode_envelope(
    bison::key_t kind,
    bison::key_t op,
    const std::string& request_id,
    const std::string& object_id,
    bool oneway,
    const std::string& payload,
    const std::string& error = {});

  /**
   * @brief Decode an RMI envelope frame.
   * @param bytes Encoded frame bytes.
   * @return Decoded envelope object.
   */
std::shared_ptr<bison::dynamic> decode_envelope(const std::vector<char>& bytes);

} // namespace bdg::bison::rmi::shared

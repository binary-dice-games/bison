// MIT License © 2025 Binary Dice Games
/**
 * @file constants.hpp
 * @brief Canonical tokens and protocol constants used by the RMI framework.
 */
#pragma once

#include "src/bison/bison.hpp"

namespace bdg::bison::rmi::shared::constants {

// ── Message kind tokens ──────────────────────────────────────────────────────
/** @brief Envelope kind token for request frames. */
inline const bison::key_t KIND_REQUEST = "request"_key;
/** @brief Envelope kind token for response frames. */
inline const bison::key_t KIND_RESPONSE = "response"_key;
/** @brief Envelope kind token for server-initiated event frames. */
inline const bison::key_t KIND_EVENT = "event"_key;

// ── Operation tokens ─────────────────────────────────────────────────────────
/** @brief Operation token for connection handshake. */
inline const bison::key_t OP_CONNECT = "connect"_key;
/** @brief Operation token for class introspection/description requests. */
inline const bison::key_t OP_DESCRIBE = "describe"_key;
/** @brief Operation token for server-side object instantiation. */
inline const bison::key_t OP_INSTANTIATE = "instantiate"_key;
/** @brief Operation token for clearing object fields. */
inline const bison::key_t OP_CLEAR = "clear"_key;
/** @brief Operation token for setting object fields. */
inline const bison::key_t OP_SET = "set"_key;
/** @brief Operation token for retrieving object fields. */
inline const bison::key_t OP_GET = "get"_key;
/** @brief Operation token for invoking remote methods. */
inline const bison::key_t OP_CALL = "call"_key;
/** @brief Operation token for destroying server-side objects. */
inline const bison::key_t OP_DESTROY = "destroy"_key;
/** @brief Operation token for disconnect requests. */
inline const bison::key_t OP_DISCONNECT = "disconnect"_key;
/** @brief Operation token for event payload dispatch. */
inline const bison::key_t OP_EVENT = "event"_key;

// ── Object lifecycle hook names
// ───────────────────────────────────────────────
/** @brief Optional method hook invoked after object instantiation. */
inline const bison::key_t HOOK_CONSTRUCT = "__construct"_key;
/** @brief Optional method hook invoked before object destruction. */
inline const bison::key_t HOOK_DESTRUCT = "__destruct"_key;
/** @brief Optional method hook invoked after `clear` requests. */
inline const bison::key_t HOOK_CLEAR = "__clear"_key;
/** @brief Optional method hook that transforms incoming `set` payloads. */
inline const bison::key_t HOOK_SETTER = "__setter"_key;
/** @brief Optional method hook that transforms outgoing `get` payloads. */
inline const bison::key_t HOOK_GETTER = "__getter"_key;

// ── Canonical error codes
// ─────────────────────────────────────────────────────
/** @brief Request envelope could not be parsed or validated. */
inline const bison::key_t ERR_INVALID_REQUEST = "INVALID_REQUEST"_key;
/** @brief Request used an unsupported protocol version. */
inline const bison::key_t ERR_UNSUPPORTED_VERSION = "UNSUPPORTED_VERSION"_key;
/** @brief Request operation token is unknown to the server. */
inline const bison::key_t ERR_UNKNOWN_OPERATION = "UNKNOWN_OPERATION"_key;
/** @brief Requested class does not exist in the registry. */
inline const bison::key_t ERR_CLASS_NOT_FOUND = "CLASS_NOT_FOUND"_key;
/** @brief Requested object ID does not exist in the session context. */
inline const bison::key_t ERR_OBJECT_NOT_FOUND = "OBJECT_NOT_FOUND"_key;
/** @brief Request is denied by policy or authorization checks. */
inline const bison::key_t ERR_ACCESS_DENIED = "ACCESS_DENIED"_key;
/** @brief Request payload failed semantic validation. */
inline const bison::key_t ERR_VALIDATION_ERROR = "VALIDATION_ERROR"_key;
/** @brief Internal server-side exception occurred while handling request. */
inline const bison::key_t ERR_INTERNAL_ERROR = "INTERNAL_ERROR"_key;
/** @brief Request timed out before completion. */
inline const bison::key_t ERR_TIMEOUT = "TIMEOUT"_key;
/** @brief Underlying transport channel failed. */
inline const bison::key_t ERR_TRANSPORT_ERROR = "TRANSPORT_ERROR"_key;

// ── Envelope class and field keys
// ─────────────────────────────────────────────
/** @brief Class key for the protocol envelope schema. */
inline const bison::key_t CLASS_ENVELOPE = "__envelope"_key;
/** @brief Class key for the protocol error schema. */
inline const bison::key_t CLASS_ERROR = "__error"_key;

/** @brief Envelope field key for protocol version. */
inline const bison::key_t FIELD_VERSION = "__version"_key;
/** @brief Envelope field key for message kind. */
inline const bison::key_t FIELD_KIND = "__kind"_key;
/** @brief Envelope field key for operation token. */
inline const bison::key_t FIELD_OP = "__op"_key;
/** @brief Envelope field key for request/response correlation ID. */
inline const bison::key_t FIELD_REQUEST_ID = "__requestId"_key;
/** @brief Envelope field key for target object identifier. */
inline const bison::key_t FIELD_OBJECT_ID = "__objectId"_key;
/** @brief Envelope field key indicating whether payload uses schema mode. */
inline const bison::key_t FIELD_WITH_SCHEMA = "__withSchema"_key;
/** @brief Envelope field key containing serialized payload bytes. */
inline const bison::key_t FIELD_PAYLOAD = "__payload"_key;
/** @brief Envelope field key containing serialized error bytes. */
inline const bison::key_t FIELD_ERROR = "__error"_key;
/** @brief Envelope field key indicating oneway request semantics. */
inline const bison::key_t FIELD_ONEWAY = "__oneway"_key;

// ── Error object field keys
// ───────────────────────────────────────────────────
/** @brief Error payload field key for canonical error code token. */
inline const bison::key_t FIELD_ERROR_CODE = "__code"_key;
/** @brief Error payload field key for human-readable message text. */
inline const bison::key_t FIELD_ERROR_MESSAGE = "__message"_key;
/** @brief Error payload field key for optional structured details. */
inline const bison::key_t FIELD_ERROR_DETAILS = "__details"_key;

// ── Operation payload field keys
// ──────────────────────────────────────────────
/** @brief Operation payload key for class token. */
inline const bison::key_t FIELD_KLASS = "__klass"_key;
/** @brief Operation payload key for class namespace token. */
inline const bison::key_t FIELD_NAMESPACE = "__namespace"_key;
/** @brief Operation payload key for argument/field payload object. */
inline const bison::key_t FIELD_PARAMS = "__params"_key;
/** @brief Operation payload key for method or event name token. */
inline const bison::key_t FIELD_NAME = "__name"_key;

// ── Protocol version ─────────────────────────────────────────────────────────
/** @brief Current wire protocol version expected by client and server. */
inline const int32_t PROTOCOL_VERSION = 1;

} // namespace bdg::bison::rmi::shared::constants

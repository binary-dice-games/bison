// MIT License © 2025 Binary Dice Games
#pragma once

#include "src/core/bison.hpp"

namespace bdg::bison::rmi::shared::constants {

// ── Message kind tokens ──────────────────────────────────────────────────────
inline const bison::key_t KIND_REQUEST  = "request"_key;
inline const bison::key_t KIND_RESPONSE = "response"_key;
inline const bison::key_t KIND_EVENT    = "event"_key;

// ── Operation tokens ─────────────────────────────────────────────────────────
inline const bison::key_t OP_CONNECT     = "connect"_key;
inline const bison::key_t OP_DESCRIBE    = "describe"_key;
inline const bison::key_t OP_INSTANTIATE = "instantiate"_key;
inline const bison::key_t OP_CLEAR       = "clear"_key;
inline const bison::key_t OP_SET         = "set"_key;
inline const bison::key_t OP_GET         = "get"_key;
inline const bison::key_t OP_CALL        = "call"_key;
inline const bison::key_t OP_DESTROY     = "destroy"_key;
inline const bison::key_t OP_DISCONNECT  = "disconnect"_key;
inline const bison::key_t OP_EVENT       = "event"_key;

// ── Object lifecycle hook names ───────────────────────────────────────────────
inline const bison::key_t HOOK_CONSTRUCT = "__construct"_key;
inline const bison::key_t HOOK_DESTRUCT  = "__destruct"_key;
inline const bison::key_t HOOK_CLEAR     = "__clear"_key;
inline const bison::key_t HOOK_SETTER    = "__setter"_key;
inline const bison::key_t HOOK_GETTER    = "__getter"_key;

// ── Canonical error codes ─────────────────────────────────────────────────────
inline const bison::key_t ERR_INVALID_REQUEST     = "INVALID_REQUEST"_key;
inline const bison::key_t ERR_UNSUPPORTED_VERSION = "UNSUPPORTED_VERSION"_key;
inline const bison::key_t ERR_UNKNOWN_OPERATION   = "UNKNOWN_OPERATION"_key;
inline const bison::key_t ERR_CLASS_NOT_FOUND     = "CLASS_NOT_FOUND"_key;
inline const bison::key_t ERR_OBJECT_NOT_FOUND    = "OBJECT_NOT_FOUND"_key;
inline const bison::key_t ERR_ACCESS_DENIED       = "ACCESS_DENIED"_key;
inline const bison::key_t ERR_VALIDATION_ERROR    = "VALIDATION_ERROR"_key;
inline const bison::key_t ERR_INTERNAL_ERROR      = "INTERNAL_ERROR"_key;
inline const bison::key_t ERR_TIMEOUT             = "TIMEOUT"_key;
inline const bison::key_t ERR_TRANSPORT_ERROR     = "TRANSPORT_ERROR"_key;

// ── Envelope class and field keys ─────────────────────────────────────────────
inline const bison::key_t CLASS_ENVELOPE   = "__envelope"_key;

inline const bison::key_t FIELD_VERSION    = "__version"_key;
inline const bison::key_t FIELD_KIND       = "__kind"_key;
inline const bison::key_t FIELD_OP         = "__op"_key;
inline const bison::key_t FIELD_REQUEST_ID = "__requestId"_key;
inline const bison::key_t FIELD_OBJECT_ID  = "__objectId"_key;
inline const bison::key_t FIELD_ONEWAY     = "__oneway"_key;
inline const bison::key_t FIELD_PAYLOAD    = "__payload"_key;
inline const bison::key_t FIELD_ERROR      = "__error"_key;

// ── Error object field keys ───────────────────────────────────────────────────
inline const bison::key_t FIELD_ERROR_CODE    = "__code"_key;
inline const bison::key_t FIELD_ERROR_MESSAGE = "__message"_key;
inline const bison::key_t FIELD_ERROR_DETAILS = "__details"_key;

// ── Operation payload field keys ──────────────────────────────────────────────
inline const bison::key_t FIELD_KLASS  = "__klass"_key;
inline const bison::key_t FIELD_PARAMS = "__params"_key;
inline const bison::key_t FIELD_NAME   = "__name"_key;

// ── Protocol version ─────────────────────────────────────────────────────────
inline const int32_t PROTOCOL_VERSION = 1;

} // namespace bdg::bison::rmi::shared::constants


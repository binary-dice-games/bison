// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and distribute this file.
// See the LICENSE file or https://opensource.org/licenses/MIT for details.

#include <bison.hpp>
#include <nlohmann/json.hpp>
#include <yaml.h>

#include <cstring>
#include <stdexcept>

using json = nlohmann::json;

namespace bdg::bison {

// ─── JSON helpers ─────────────────────────────────────────────────────────────

std::shared_ptr<dynamic> from_json_array(json::array_t data);
std::shared_ptr<dynamic> from_json_object(json::object_t data);

std::shared_ptr<dynamic> from_json_array(json::array_t data) {
  size_t idx = 0;
  auto dyn = dynamic_ptr{};
  for (auto it = data.begin(); it != data.end(); ++it) {
    switch (it->type()) {
      case json::value_t::null:
        (*dyn)[idx++] = std::shared_ptr<dynamic>{};
        break;
      case json::value_t::boolean:
        (*dyn)[idx++] = bool{*it};
        break;
      case json::value_t::number_integer:
        (*dyn)[idx++] = int32_t{*it};
        break;
      case json::value_t::number_unsigned:
        (*dyn)[idx++] = int32_t{*it};
        break;
      case json::value_t::number_float:
        (*dyn)[idx++] = float{*it};
        break;
      case json::value_t::string:
        (*dyn)[idx++] = std::string{*it};
        break;
      case json::value_t::array:
        (*dyn)[idx++] = from_json_array(*it);
        break;
      case json::value_t::object:
        (*dyn)[idx++] = from_json_object(*it);
        break;
    }
  }

  return dyn;
}

std::shared_ptr<dynamic> from_json_object(json::object_t data) {
  auto dyn = dynamic_ptr{};
  for (auto it = data.begin(); it != data.end(); ++it) {
    switch (it->second.type()) {
      case json::value_t::null:
        (*dyn)[it->first] = std::shared_ptr<dynamic>{};
        break;
      case json::value_t::boolean:
        (*dyn)[it->first] = bool{it->second};
        break;
      case json::value_t::number_integer:
        (*dyn)[it->first] = int32_t{it->second};
        break;
      case json::value_t::number_unsigned:
        (*dyn)[it->first] = int32_t{it->second};
        break;
      case json::value_t::number_float:
        (*dyn)[it->first] = float{it->second};
        break;
      case json::value_t::string:
        (*dyn)[it->first] = std::string{it->second};
        break;
      case json::value_t::array:
        (*dyn)[it->first] = from_json_array(it->second);
        break;
      case json::value_t::object:
        (*dyn)[it->first] = from_json_object(it->second);
        break;
    }
  }

  return dyn;
}

// ─── YAML helpers ─────────────────────────────────────────────────────────────

/**
 * @brief Parse a YAML scalar string into the most specific field type.
 *
 * The coercion rules are applied in order:
 *  - "null", "~", "" (plain)   → null `shared_ptr<dynamic>`
 *  - "true" / "false"          → `bool`
 *  - parseable as `int`        → `int32_t`
 *  - parseable as `double`     → `float`
 *  - everything else           → `std::string`
 *
 * @param value  The raw scalar text from the YAML stream.
 * @param plain  True if the scalar was unquoted (plain style).
 */
static field yaml_scalar_to_field(const char* value, bool plain) {
  if (plain) {
    if (std::strcmp(value, "null") == 0 || std::strcmp(value, "~") == 0 ||
        std::strcmp(value, "") == 0) {
      return field{std::shared_ptr<dynamic>{}};
    }
    if (std::strcmp(value, "true") == 0 || std::strcmp(value, "yes") == 0 ||
        std::strcmp(value, "on") == 0) {
      return field{true};
    }
    if (std::strcmp(value, "false") == 0 || std::strcmp(value, "no") == 0 ||
        std::strcmp(value, "off") == 0) {
      return field{false};
    }
    // Try integer
    char* end = nullptr;
    long iv = std::strtol(value, &end, 10);
    if (end != value && *end == '\0') {
      return field{static_cast<int32_t>(iv)};
    }
    // Try float
    double dv = std::strtod(value, &end);
    if (end != value && *end == '\0') {
      return field{static_cast<float>(dv)};
    }
  }
  return field{std::string(value)};
}

// Forward declarations for mutual recursion.
static std::shared_ptr<dynamic> yaml_parse_mapping(yaml_parser_t* parser);
static std::shared_ptr<dynamic> yaml_parse_sequence(yaml_parser_t* parser);
static field yaml_parse_value(yaml_parser_t* parser, const yaml_event_t* ev);

/**
 * @brief Consume events for the value associated with a mapping key or
 *        sequence element and return the corresponding `field`.
 *
 * @p ev holds the *already consumed* event that started the value.
 */
static field yaml_parse_value(yaml_parser_t* parser, const yaml_event_t* ev) {
  switch (ev->type) {
    case YAML_SCALAR_EVENT: {
      bool plain = ev->data.scalar.style == YAML_PLAIN_SCALAR_STYLE;
      return yaml_scalar_to_field(
          reinterpret_cast<const char*>(ev->data.scalar.value), plain);
    }
    case YAML_MAPPING_START_EVENT:
      return field{yaml_parse_mapping(parser)};
    case YAML_SEQUENCE_START_EVENT:
      return field{yaml_parse_sequence(parser)};
    default:
      return field{std::monostate{}};
  }
}

static std::shared_ptr<dynamic> yaml_parse_mapping(yaml_parser_t* parser) {
  auto dyn = dynamic_ptr{};
  yaml_event_t ev;
  while (true) {
    if (!yaml_parser_parse(parser, &ev)) {
      throw std::runtime_error("YAML parse error in mapping");
    }
    yaml_event_type_t type = ev.type;
    if (type == YAML_MAPPING_END_EVENT) {
      yaml_event_delete(&ev);
      break;
    }
    // Key must be a scalar.
    if (type != YAML_SCALAR_EVENT) {
      yaml_event_delete(&ev);
      throw std::runtime_error("YAML mapping key must be a scalar");
    }
    std::string key(reinterpret_cast<const char*>(ev.data.scalar.value));
    yaml_event_delete(&ev);

    // Parse value.
    if (!yaml_parser_parse(parser, &ev)) {
      throw std::runtime_error("YAML parse error reading mapping value");
    }
    (*dyn)[key] = yaml_parse_value(parser, &ev);
    yaml_event_delete(&ev);
  }
  return dyn;
}

static std::shared_ptr<dynamic> yaml_parse_sequence(yaml_parser_t* parser) {
  auto dyn = dynamic_ptr{};
  size_t idx = 0;
  yaml_event_t ev;
  while (true) {
    if (!yaml_parser_parse(parser, &ev)) {
      throw std::runtime_error("YAML parse error in sequence");
    }
    yaml_event_type_t type = ev.type;
    if (type == YAML_SEQUENCE_END_EVENT) {
      yaml_event_delete(&ev);
      break;
    }
    (*dyn)[idx++] = yaml_parse_value(parser, &ev);
    yaml_event_delete(&ev);
  }
  return dyn;
}

// ─── MessagePack helpers ──────────────────────────────────────────────────────

/**
 * @brief Read one byte from a buffer.
 */
static uint8_t msgpack_read_byte(const uint8_t* buf, size_t len, size_t& pos) {
  if (pos >= len) throw std::runtime_error("MessagePack: unexpected end of data");
  return buf[pos++];
}

static uint16_t msgpack_read_u16(const uint8_t* buf, size_t len, size_t& pos) {
  if (pos + 2 > len) throw std::runtime_error("MessagePack: unexpected end of data");
  uint16_t v = (uint16_t(buf[pos]) << 8) | buf[pos + 1];
  pos += 2;
  return v;
}

static uint32_t msgpack_read_u32(const uint8_t* buf, size_t len, size_t& pos) {
  if (pos + 4 > len) throw std::runtime_error("MessagePack: unexpected end of data");
  uint32_t v = (uint32_t(buf[pos]) << 24) | (uint32_t(buf[pos + 1]) << 16) |
               (uint32_t(buf[pos + 2]) << 8) | buf[pos + 3];
  pos += 4;
  return v;
}

// Forward declaration.
static field msgpack_parse_value(const uint8_t* buf, size_t len, size_t& pos);

static std::string msgpack_read_str(const uint8_t* buf, size_t len, size_t& pos,
                                    size_t n) {
  if (pos + n > len) throw std::runtime_error("MessagePack: unexpected end of data");
  std::string s(reinterpret_cast<const char*>(buf + pos), n);
  pos += n;
  return s;
}

static std::shared_ptr<dynamic> msgpack_parse_map(const uint8_t* buf, size_t len,
                                                   size_t& pos, size_t count) {
  auto dyn = dynamic_ptr{};
  for (size_t i = 0; i < count; ++i) {
    // Key: must be a string or integer for our purposes.
    field key_f = msgpack_parse_value(buf, len, pos);
    std::string key;
    if (key_f.is<std::string>()) {
      key = std::string(key_f);
    } else if (key_f.is<int32_t>()) {
      key = std::to_string(int32_t(key_f));
    } else {
      throw std::runtime_error("MessagePack: unsupported map key type");
    }
    (*dyn)[key] = msgpack_parse_value(buf, len, pos);
  }
  return dyn;
}

static std::shared_ptr<dynamic> msgpack_parse_array(const uint8_t* buf, size_t len,
                                                     size_t& pos, size_t count) {
  auto dyn = dynamic_ptr{};
  for (size_t i = 0; i < count; ++i) {
    (*dyn)[i] = msgpack_parse_value(buf, len, pos);
  }
  return dyn;
}

static field msgpack_parse_value(const uint8_t* buf, size_t len, size_t& pos) {
  uint8_t b = msgpack_read_byte(buf, len, pos);

  // Positive fixint (0x00–0x7f)
  if (b <= 0x7f) return field{static_cast<int32_t>(b)};
  // Fixmap (0x80–0x8f)
  if ((b & 0xf0) == 0x80)
    return field{msgpack_parse_map(buf, len, pos, b & 0x0f)};
  // Fixarray (0x90–0x9f)
  if ((b & 0xf0) == 0x90)
    return field{msgpack_parse_array(buf, len, pos, b & 0x0f)};
  // Fixstr (0xa0–0xbf)
  if ((b & 0xe0) == 0xa0)
    return field{msgpack_read_str(buf, len, pos, b & 0x1f)};
  // Negative fixint (0xe0–0xff)
  if (b >= 0xe0) return field{static_cast<int32_t>(static_cast<int8_t>(b))};

  switch (b) {
    case 0xc0: return field{std::shared_ptr<dynamic>{}};       // nil
    case 0xc2: return field{false};                            // false
    case 0xc3: return field{true};                             // true
    case 0xca: {                                               // float32
      uint32_t u = msgpack_read_u32(buf, len, pos);
      float v;
      std::memcpy(&v, &u, sizeof v);
      return field{v};
    }
    case 0xcc: return field{static_cast<int32_t>(msgpack_read_byte(buf, len, pos))};  // uint8
    case 0xcd: return field{static_cast<int32_t>(msgpack_read_u16(buf, len, pos))};  // uint16
    case 0xce: return field{static_cast<int32_t>(msgpack_read_u32(buf, len, pos))};  // uint32
    case 0xd0: return field{static_cast<int32_t>(static_cast<int8_t>(msgpack_read_byte(buf, len, pos)))};  // int8
    case 0xd1: return field{static_cast<int32_t>(static_cast<int16_t>(msgpack_read_u16(buf, len, pos)))};  // int16
    case 0xd2: return field{static_cast<int32_t>(msgpack_read_u32(buf, len, pos))};  // int32
    case 0xd9: {  // str8
      uint8_t n = msgpack_read_byte(buf, len, pos);
      return field{msgpack_read_str(buf, len, pos, n)};
    }
    case 0xda: {  // str16
      uint16_t n = msgpack_read_u16(buf, len, pos);
      return field{msgpack_read_str(buf, len, pos, n)};
    }
    case 0xdb: {  // str32
      uint32_t n = msgpack_read_u32(buf, len, pos);
      return field{msgpack_read_str(buf, len, pos, n)};
    }
    case 0xdc: {  // array16
      uint16_t n = msgpack_read_u16(buf, len, pos);
      return field{msgpack_parse_array(buf, len, pos, n)};
    }
    case 0xdd: {  // array32
      uint32_t n = msgpack_read_u32(buf, len, pos);
      return field{msgpack_parse_array(buf, len, pos, n)};
    }
    case 0xde: {  // map16
      uint16_t n = msgpack_read_u16(buf, len, pos);
      return field{msgpack_parse_map(buf, len, pos, n)};
    }
    case 0xdf: {  // map32
      uint32_t n = msgpack_read_u32(buf, len, pos);
      return field{msgpack_parse_map(buf, len, pos, n)};
    }
    default:
      throw std::runtime_error(
          std::string("MessagePack: unsupported format byte 0x") +
          std::to_string(b));
  }
}

// ─── Public extension functions ───────────────────────────────────────────────

namespace extensions {

std::shared_ptr<dynamic> from_json(std::string text) {
  json data = json::parse(text);
  return from_json_object(data);
}

std::shared_ptr<dynamic> from_yaml(std::string text) {
  yaml_parser_t parser;
  if (!yaml_parser_initialize(&parser)) {
    throw std::runtime_error("Failed to initialize YAML parser");
  }

  yaml_parser_set_input_string(
      &parser,
      reinterpret_cast<const unsigned char*>(text.c_str()),
      text.size());

  std::shared_ptr<dynamic> result;
  yaml_event_t ev;
  bool done = false;

  while (!done) {
    if (!yaml_parser_parse(&parser, &ev)) {
      std::string msg = parser.problem ? parser.problem : "unknown error";
      yaml_parser_delete(&parser);
      throw std::runtime_error("YAML parse error: " + msg);
    }
    switch (ev.type) {
      case YAML_STREAM_END_EVENT:
        done = true;
        yaml_event_delete(&ev);
        break;
      case YAML_MAPPING_START_EVENT:
        result = yaml_parse_mapping(&parser);
        yaml_event_delete(&ev);
        break;
      case YAML_SEQUENCE_START_EVENT:
        result = yaml_parse_sequence(&parser);
        yaml_event_delete(&ev);
        break;
      default:
        yaml_event_delete(&ev);
        break;
    }
  }

  yaml_parser_delete(&parser);
  if (!result) result = dynamic_ptr{};
  return result;
}

std::shared_ptr<dynamic> from_msgpack(const uint8_t* data, size_t len) {
  size_t pos = 0;
  field f = msgpack_parse_value(data, len, pos);
  if (f.is<std::shared_ptr<dynamic>>()) {
    auto sp = std::shared_ptr<dynamic>(f);
    return sp ? sp : dynamic_ptr{};
  }
  // Top-level scalar: wrap in a dynamic at index 0.
  auto dyn = dynamic_ptr{};
  (*dyn)[size_t{0}] = f;
  return dyn;
}

} // namespace extensions
} // namespace bdg::bison
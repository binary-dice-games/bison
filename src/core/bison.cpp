// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

#include <bison.hpp>
#include <nlohmann/json.hpp>
#include <yaml.h>

#include <stdexcept>

using json = nlohmann::json;

namespace bdg::bison {

// ─── JSON helpers
// ─────────────────────────────────────────────────────────────

dynamic_ptr from_json_array(json::array_t data);
dynamic_ptr from_json_object(json::object_t data);

dynamic_ptr from_json_array(json::array_t data) {
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

dynamic_ptr from_json_object(json::object_t data) {
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

// ─── YAML helpers
// ─────────────────────────────────────────────────────────────

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
static dynamic_ptr yaml_parse_mapping(yaml_parser_t* parser);
static dynamic_ptr yaml_parse_sequence(yaml_parser_t* parser);
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

static dynamic_ptr yaml_parse_mapping(yaml_parser_t* parser) {
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

static dynamic_ptr yaml_parse_sequence(yaml_parser_t* parser) {
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

// ─── Public extension functions
// ───────────────────────────────────────────────

namespace extensions {

dynamic_ptr from_json(std::string text) {
  json data = json::parse(text);
  return from_json_object(data);
}

dynamic_ptr from_yaml(std::string text) {
  yaml_parser_t parser;
  if (!yaml_parser_initialize(&parser)) {
    throw std::runtime_error("Failed to initialize YAML parser");
  }

  yaml_parser_set_input_string(
      &parser,
      reinterpret_cast<const unsigned char*>(text.c_str()),
      text.size());

  dynamic_ptr result;
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
  if (!result)
    result = dynamic_ptr{};
  return result;
}

} // namespace extensions
} // namespace bdg::bison
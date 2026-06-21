// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

/**
 * @file bison.cpp
 * @brief Implementations of the JSON and YAML import extensions for the Bison
 *        core library.
 *
 * Provides `bdg::bison::extensions::from_json` (using nlohmann/json) and
 * `bdg::bison::extensions::from_yaml` (using libyaml event-stream parsing).
 * Both functions return a `dynamic_ptr` representing the root of the parsed
 * document.
 */

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

// ─── JSON export helpers
// ──────────────────────────────────────────────────

using key_map = std::unordered_map<uint32_t, std::string>;

static json field_to_json(const field& f, const key_map& keys);
static json dynamic_to_json(const dynamic& d, const key_map& keys);

static std::string resolve_key(uint32_t id, const key_map& keys) {
  auto it = keys.find(id);
  return (it != keys.end()) ? it->second : ('#' + std::to_string(id));
}

static json field_to_json(const field& f, const key_map& keys) {
  return std::visit(
      [&](const auto& v) -> json {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          return nullptr;
        } else if constexpr (std::is_same_v<T, bool>) {
          return v;
        } else if constexpr (std::is_same_v<T, int32_t>) {
          return v;
        } else if constexpr (std::is_same_v<T, float>) {
          return v;
        } else if constexpr (std::is_same_v<T, std::string>) {
          return v;
        } else if constexpr (std::is_same_v<T, hash_t>) {
          return resolve_key(v, keys);
        } else if constexpr (std::is_same_v<T, key_t>) {
          return resolve_key(v.id, keys);
        } else if constexpr (std::is_same_v<T, dynamic_ptr>) {
          return v ? dynamic_to_json(*v, keys) : json(nullptr);
        } else if constexpr (std::is_same_v<T, std::vector<bool>>) {
          auto arr = json::array();
          for (bool b : v) arr.push_back(b);
          return arr;
        } else if constexpr (
            std::is_same_v<T, std::vector<int32_t>> ||
            std::is_same_v<T, std::vector<float>>) {
          return json(v);
        } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
          // Render raw bytes as lowercase hex string.
          std::ostringstream oss;
          oss << std::hex << std::setfill('0');
          for (uint8_t b : v)
            oss << std::setw(2) << static_cast<int>(b);
          return oss.str();
        } else {
          return nullptr;
        }
      },
      static_cast<const field_base&>(f));
}

static json dynamic_to_json(const dynamic& d, const key_map& keys) {
  auto obj = json::object();
  d.forEach([&](key_t k, const field& f) {
    obj[resolve_key(k.id, keys)] = field_to_json(f, keys);
  });
  return obj;
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

std::string to_json(const dynamic& d,
                    const std::unordered_map<uint32_t, std::string>& keys,
                    int indent) {
  return dynamic_to_json(d, keys).dump(indent);
}

// ─── YAML export helpers
// ──────────────────────────────────────────────────

namespace {

static void yaml_send(yaml_emitter_t* e, yaml_event_t& ev) {
  if (!yaml_emitter_emit(e, &ev))
    throw std::runtime_error(
        std::string("YAML emit: ") + (e->problem ? e->problem : "unknown"));
}

static void yaml_scalar_plain(yaml_emitter_t* e, const std::string& s) {
  yaml_event_t ev;
  yaml_scalar_event_initialize(
      &ev, nullptr, nullptr,
      reinterpret_cast<const yaml_char_t*>(s.c_str()),
      static_cast<int>(s.size()),
      /*plain_implicit=*/1, /*quoted_implicit=*/0, YAML_PLAIN_SCALAR_STYLE);
  yaml_send(e, ev);
}

static void yaml_scalar_quoted(yaml_emitter_t* e, const std::string& s) {
  yaml_event_t ev;
  yaml_scalar_event_initialize(
      &ev, nullptr, nullptr,
      reinterpret_cast<const yaml_char_t*>(s.c_str()),
      static_cast<int>(s.size()),
      /*plain_implicit=*/0, /*quoted_implicit=*/1,
      YAML_DOUBLE_QUOTED_SCALAR_STYLE);
  yaml_send(e, ev);
}

static std::string format_float_yaml(float f) {
  std::ostringstream oss;
  oss << std::setprecision(7) << f;
  std::string s = oss.str();
  if (s.find('.') == std::string::npos && s.find('e') == std::string::npos)
    s += ".0";
  return s;
}

template <typename Vec, typename Fmt>
static void yaml_emit_sequence(yaml_emitter_t* e, const Vec& v, Fmt fmt) {
  yaml_event_t ev;
  yaml_sequence_start_event_initialize(
      &ev, nullptr, nullptr, /*implicit=*/1, YAML_BLOCK_SEQUENCE_STYLE);
  yaml_send(e, ev);
  for (const auto& item : v)
    yaml_scalar_plain(e, fmt(item));
  yaml_sequence_end_event_initialize(&ev);
  yaml_send(e, ev);
}

static void emit_field_yaml(
    yaml_emitter_t* e, const field& f, const key_map& keys);
static void emit_dynamic_yaml(
    yaml_emitter_t* e, const dynamic& d, const key_map& keys);

static void emit_field_yaml(
    yaml_emitter_t* e, const field& f, const key_map& keys) {
  std::visit(
      [&](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          yaml_scalar_plain(e, "null");
        } else if constexpr (std::is_same_v<T, bool>) {
          yaml_scalar_plain(e, v ? "true" : "false");
        } else if constexpr (std::is_same_v<T, int32_t>) {
          yaml_scalar_plain(e, std::to_string(v));
        } else if constexpr (std::is_same_v<T, float>) {
          yaml_scalar_plain(e, format_float_yaml(v));
        } else if constexpr (std::is_same_v<T, std::string>) {
          yaml_scalar_quoted(e, v);
        } else if constexpr (std::is_same_v<T, hash_t>) {
          yaml_scalar_plain(e, resolve_key(v, keys));
        } else if constexpr (std::is_same_v<T, key_t>) {
          yaml_scalar_plain(e, resolve_key(v.id, keys));
        } else if constexpr (std::is_same_v<T, dynamic_ptr>) {
          if (v)
            emit_dynamic_yaml(e, *v, keys);
          else
            yaml_scalar_plain(e, "null");
        } else if constexpr (std::is_same_v<T, std::vector<bool>>) {
          yaml_emit_sequence(e, v, [](bool b) { return b ? "true" : "false"; });
        } else if constexpr (std::is_same_v<T, std::vector<int32_t>>) {
          yaml_emit_sequence(e, v, [](int32_t i) { return std::to_string(i); });
        } else if constexpr (std::is_same_v<T, std::vector<float>>) {
          yaml_emit_sequence(e, v, [](float f) { return format_float_yaml(f); });
        } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
          // Render raw bytes as lowercase hex string.
          std::ostringstream oss;
          oss << std::hex << std::setfill('0');
          for (uint8_t b : v)
            oss << std::setw(2) << static_cast<int>(b);
          yaml_scalar_quoted(e, oss.str());
        }
      },
      static_cast<const field_base&>(f));
}

static void emit_dynamic_yaml(
    yaml_emitter_t* e, const dynamic& d, const key_map& keys) {
  yaml_event_t ev;
  yaml_mapping_start_event_initialize(
      &ev, nullptr, nullptr, /*implicit=*/1, YAML_BLOCK_MAPPING_STYLE);
  yaml_send(e, ev);
  d.forEach([&](key_t k, const field& f) {
    yaml_scalar_plain(e, resolve_key(k.id, keys));
    emit_field_yaml(e, f, keys);
  });
  yaml_mapping_end_event_initialize(&ev);
  yaml_send(e, ev);
}

} // namespace

std::string to_yaml(const dynamic& d,
                    const std::unordered_map<uint32_t, std::string>& keys) {
  // Set up emitter with a write callback that appends to a std::string.
  yaml_emitter_t emitter;
  std::string output;
  if (!yaml_emitter_initialize(&emitter))
    throw std::runtime_error("YAML emitter: failed to initialize");

  struct guard_t {
    yaml_emitter_t& e;
    ~guard_t() { yaml_emitter_delete(&e); }
  } guard{emitter};

  yaml_emitter_set_output(
      &emitter,
      [](void* data, unsigned char* buf, size_t size) -> int {
        static_cast<std::string*>(data)->append(
            reinterpret_cast<char*>(buf), size);
        return 1;
      },
      &output);
  yaml_emitter_set_unicode(&emitter, 1);

  yaml_event_t ev;
  yaml_stream_start_event_initialize(&ev, YAML_UTF8_ENCODING);
  yaml_send(&emitter, ev);
  yaml_document_start_event_initialize(&ev, nullptr, nullptr, nullptr, /*implicit=*/1);
  yaml_send(&emitter, ev);

  emit_dynamic_yaml(&emitter, d, keys);

  yaml_document_end_event_initialize(&ev, /*implicit=*/1);
  yaml_send(&emitter, ev);
  yaml_stream_end_event_initialize(&ev);
  yaml_send(&emitter, ev);

  return output;
}

} // namespace extensions

} // namespace bdg::bison
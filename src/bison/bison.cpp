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
#include <gflags/gflags.h>
#include <nlohmann/json.hpp>
#include <yaml.h>

#include <stdexcept>

using json = nlohmann::json;

namespace bdg::bison {

// ─── JSON helpers
// ─────────────────────────────────────────────────────────────

field from_json_array_field(const json::array_t& data);
dynamic_ptr from_json_array(json::array_t data);
dynamic_ptr from_json_object(json::object_t data);

/**
 * @brief Classify a JSON array and convert it to the most specific `field`
 *        type that reproduces what `bison_to_json` would have emitted.
 *
 * `to_json` renders `std::vector<bool>`, `std::vector<int32_t>`, and
 * `std::vector<float>` fields as plain JSON arrays of the corresponding
 * scalar type (see `field_to_json`). To round-trip those fields, a
 * homogeneous array is reconstructed as the matching vector type:
 *  - all booleans        → `std::vector<bool>`
 *  - all integers        → `std::vector<int32_t>`
 *  - all numeric, any float → `std::vector<float>`
 *
 * Any other array (empty, mixed types, or containing strings/objects/
 * nested arrays/nulls) falls back to an indexed `dynamic_ptr`, matching
 * the pre-existing behavior for arbitrary JSON input.
 */
field from_json_array_field(const json::array_t& data) {
  if (!data.empty()) {
    bool all_bool = true;
    bool all_int = true;
    bool all_numeric = true;
    for (const auto& el : data) {
      const auto t = el.type();
      const bool is_int = t == json::value_t::number_integer || t == json::value_t::number_unsigned;
      const bool is_float = t == json::value_t::number_float;
      all_bool = all_bool && t == json::value_t::boolean;
      all_int = all_int && is_int;
      all_numeric = all_numeric && (is_int || is_float);
    }
    if (all_bool) {
      std::vector<bool> v;
      v.reserve(data.size());
      for (const auto& el : data)
        v.push_back(bool{el});
      return field{std::move(v)};
    }
    if (all_int) {
      std::vector<int32_t> v;
      v.reserve(data.size());
      for (const auto& el : data)
        v.push_back(int32_t{el});
      return field{std::move(v)};
    }
    if (all_numeric) {
      std::vector<float> v;
      v.reserve(data.size());
      for (const auto& el : data)
        v.push_back(float{el});
      return field{std::move(v)};
    }
  }
  return field{from_json_array(data)};
}

/**
 * @brief Convert a JSON array into an indexed `dynamic_ptr`.
 *
 * Every element becomes a numeric-index field (`0`, `1`, `2`, ...), with
 * nested arrays and objects recursively converted via
 * `from_json_array_field` and `from_json_object` respectively. This is the
 * fallback representation used for arrays that are not homogeneous enough
 * to become one of the vector field types (see `from_json_array_field`).
 *
 * @param data The parsed JSON array.
 * @return An indexed `dynamic_ptr` with one field per array element.
 */
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
        (*dyn)[idx++] = from_json_array_field(*it);
        break;
      case json::value_t::object:
        (*dyn)[idx++] = from_json_object(*it);
        break;
    }
  }

  return dyn;
}

/**
 * @brief Convert a JSON object into a named-field `dynamic_ptr`.
 *
 * Each key/value pair becomes a named field, with nested arrays and
 * objects recursively converted via `from_json_array_field` and
 * `from_json_object` respectively. Used both for the document root
 * (`extensions::from_json`) and for nested objects encountered while
 * walking a document.
 *
 * @param data The parsed JSON object.
 * @return A `dynamic_ptr` with one named field per JSON key.
 */
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
        (*dyn)[it->first] = from_json_array_field(it->second);
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
    if (std::strcmp(value, "null") == 0 || std::strcmp(value, "~") == 0 || std::strcmp(value, "") == 0) {
      return field{std::shared_ptr<dynamic>{}};
    }
    if (std::strcmp(value, "true") == 0 || std::strcmp(value, "yes") == 0 || std::strcmp(value, "on") == 0) {
      return field{true};
    }
    if (std::strcmp(value, "false") == 0 || std::strcmp(value, "no") == 0 || std::strcmp(value, "off") == 0) {
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
static field yaml_sequence_to_field(yaml_parser_t* parser);
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
      return yaml_scalar_to_field(reinterpret_cast<const char*>(ev->data.scalar.value), plain);
    }
    case YAML_MAPPING_START_EVENT:
      return field{yaml_parse_mapping(parser)};
    case YAML_SEQUENCE_START_EVENT:
      return yaml_sequence_to_field(parser);
    default:
      return field{std::monostate{}};
  }
}

/**
 * @brief Consume a YAML mapping's events and return it as a named-field
 *        `dynamic_ptr`.
 *
 * Called with the `YAML_MAPPING_START_EVENT` already consumed; reads
 * key/value event pairs until the matching `YAML_MAPPING_END_EVENT`.
 * Keys must be scalars; values are parsed recursively via
 * `yaml_parse_value`.
 */
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

/**
 * @brief Consume a YAML sequence's events and return its elements as a flat
 *        list of already-parsed `field` values (nested mappings/sequences
 *        are fully resolved).
 */
static std::vector<field> yaml_collect_sequence_items(yaml_parser_t* parser) {
  std::vector<field> items;
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
    items.push_back(yaml_parse_value(parser, &ev));
    yaml_event_delete(&ev);
  }
  return items;
}

/** @brief Build an indexed `dynamic_ptr` (array-like) from parsed items. */
static dynamic_ptr yaml_items_to_indexed_dynamic(std::vector<field>& items) {
  auto dyn = dynamic_ptr{};
  size_t idx = 0;
  for (auto& item : items)
    (*dyn)[idx++] = std::move(item);
  return dyn;
}

/**
 * @brief Consume a YAML sequence's events and return it as an indexed
 *        `dynamic_ptr`, unconditionally (no vector-type classification).
 *
 * Used only for the document root, since `bison_to_yaml` always emits a
 * mapping there — a top-level sequence is necessarily hand-authored YAML,
 * so it keeps the simple array-like representation rather than attempting
 * to reconstruct a vector field type. Nested sequences use
 * `yaml_sequence_to_field` instead.
 */
static dynamic_ptr yaml_parse_sequence(yaml_parser_t* parser) {
  auto items = yaml_collect_sequence_items(parser);
  return yaml_items_to_indexed_dynamic(items);
}

/**
 * @brief Classify a nested YAML sequence and convert it to the most
 *        specific `field` type that reproduces what `bison_to_yaml` would
 *        have emitted.
 *
 * `to_yaml` renders `std::vector<bool>`, `std::vector<int32_t>`, and
 * `std::vector<float>` fields as plain block sequences of the
 * corresponding scalar type (see `emit_field_yaml`). To round-trip those
 * fields, a homogeneous sequence is reconstructed as the matching vector
 * type:
 *  - all booleans           → `std::vector<bool>`
 *  - all integers           → `std::vector<int32_t>`
 *  - all numeric, any float → `std::vector<float>`
 *
 * Any other sequence (empty, mixed types, or containing strings/mappings/
 * nested sequences/nulls) falls back to an indexed `dynamic_ptr`, matching
 * the pre-existing behavior for arbitrary YAML input.
 */
static field yaml_sequence_to_field(yaml_parser_t* parser) {
  auto items = yaml_collect_sequence_items(parser);
  if (!items.empty()) {
    bool all_bool = true;
    bool all_int = true;
    bool all_numeric = true;
    for (const auto& f : items) {
      const auto& base = static_cast<const field_base&>(f);
      const bool is_int = std::holds_alternative<int32_t>(base);
      const bool is_float = std::holds_alternative<float>(base);
      all_bool = all_bool && std::holds_alternative<bool>(base);
      all_int = all_int && is_int;
      all_numeric = all_numeric && (is_int || is_float);
    }
    if (all_bool) {
      std::vector<bool> v;
      v.reserve(items.size());
      for (auto& f : items)
        v.push_back(std::get<bool>(static_cast<field_base&>(f)));
      return field{std::move(v)};
    }
    if (all_int) {
      std::vector<int32_t> v;
      v.reserve(items.size());
      for (auto& f : items)
        v.push_back(std::get<int32_t>(static_cast<field_base&>(f)));
      return field{std::move(v)};
    }
    if (all_numeric) {
      std::vector<float> v;
      v.reserve(items.size());
      for (auto& f : items) {
        const auto& base = static_cast<const field_base&>(f);
        if (const auto* i = std::get_if<int32_t>(&base))
          v.push_back(static_cast<float>(*i));
        else
          v.push_back(std::get<float>(base));
      }
      return field{std::move(v)};
    }
  }
  return field{yaml_items_to_indexed_dynamic(items)};
}

/**
 * @brief True if @p k is the reserved `__class`/`__namespace` bookkeeping
 *        field and @p f holds the "unset" sentinel value `0` -- `dynamic`'s
 *        default "anonymous object" class, or the global namespace.
 *
 * Every `dynamic` gets an explicit `__class` field at construction time
 * (see the `dynamic` constructor), and `resolveNamespace()` lazily caches
 * an explicit `__namespace` field the first time it's consulted -- both
 * default to `0` when the object was never assigned a real class/namespace.
 * That `0` carries no identifying information (there is no class or
 * namespace named "0"), so `print()`, `to_json()`, and `to_yaml()` omit the
 * field entirely instead of rendering a meaningless `"#0"`. A non-zero
 * value (a real registered class/namespace hash) is still emitted.
 */
static bool is_zero_bookkeeping_field_p(key_t k, const field& f) {
  const hash_t h = static_cast<hash_t>(k);
  if (h != dynamic::CLASS && h != dynamic::NAMESPACE)
    return false;
  return f.is<key_t>() && f.as<key_t>().id == 0;
}

// ─── JSON export helpers
// ──────────────────────────────────────────────────

using key_map = std::unordered_map<uint32_t, std::string>;

static json field_to_json(const field& f, const key_map& keys);
static json dynamic_to_json(const dynamic& d, const key_map& keys);

/**
 * @brief Resolve a field/hash key id to a human-readable name for export.
 *
 * Looks @p id up in @p keys, which the caller of `to_json` / `to_yaml`
 * supplies. This name must be the exact original field-name string --
 * never a `DisplayName` attribute -- so that re-importing the exported
 * document via `from_json`/`from_yaml` (which re-hashes the key string)
 * reproduces the original key. Falls back to a `#<id>` placeholder when no
 * name is registered, so the key is still stable and greppable in the
 * exported document.
 *
 * @param id   Raw `hash_t` key id.
 * @param keys Id-to-name lookup table.
 * @return Resolved display name, or `"#<id>"` if unknown.
 */
static std::string resolve_key(uint32_t id, const key_map& keys) {
  auto it = keys.find(id);
  return (it != keys.end()) ? it->second : ('#' + std::to_string(id));
}

/**
 * @brief Convert a single `field` to its JSON representation.
 *
 * Dispatches on the active `field_base` alternative: scalars map to the
 * corresponding JSON scalar type, `dynamic_ptr` recurses via
 * `dynamic_to_json`, and the vector types are rendered the way described
 * in `from_json_array_field` (bool/int32/float arrays as plain JSON
 * arrays, `vector<uint8_t>` as a lowercase hex string).
 *
 * @param f    Field to convert.
 * @param keys Id-to-name lookup used for `hash_t` / `key_t` fields.
 * @return The JSON value equivalent to @p f.
 */
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
          for (bool b : v)
            arr.push_back(b);
          return arr;
        } else if constexpr (std::is_same_v<T, std::vector<int32_t>> || std::is_same_v<T, std::vector<float>>) {
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

/**
 * @brief Convert a `dynamic` object's named fields to a JSON object.
 *
 * Only named fields are visited (via `dynamic::forEach`); methods and
 * attributes are not exported. This always produces a JSON object, even
 * for a `dynamic` that is being used array-like (numeric-indexed fields
 * are keyed by their resolved index name, not rendered as a JSON array).
 * `__class`/`__namespace` fields holding the `0` "unset" sentinel are
 * skipped (see `is_zero_bookkeeping_field_p`).
 *
 * @param d    Object to convert.
 * @param keys Id-to-name lookup used to resolve field keys.
 * @return A JSON object with one entry per field in @p d.
 */
static json dynamic_to_json(const dynamic& d, const key_map& keys) {
  auto obj = json::object();
  d.forEach([&](key_t k, const field& f) {
    if (is_zero_bookkeeping_field_p(k, f))
      return;
    obj[resolve_key(k.id, keys)] = field_to_json(f, keys);
  });
  return obj;
}

// ─── Public extension functions
// ───────────────────────────────────────────────

namespace extensions {

/**
 * @brief Parse a JSON document into a `dynamic_ptr`.
 *
 * The document root must be a JSON object (matching what `to_json`
 * always emits); throws if @p text is not valid JSON or its root is not
 * an object.
 *
 * @param text UTF-8 JSON document text.
 * @return Root `dynamic_ptr` of the parsed document.
 */
dynamic_ptr from_json(std::string text) {
  json data = json::parse(text);
  return from_json_object(data);
}

/**
 * @brief Parse a YAML document into a `dynamic_ptr`.
 *
 * Drives a libyaml event-stream parser and dispatches the first
 * document-level event: a mapping is parsed via `yaml_parse_mapping`, a
 * sequence via `yaml_parse_sequence` (see that function for why top-level
 * sequences are not classified into vector fields). Any other root event
 * (e.g. a bare scalar) yields an empty `dynamic_ptr`.
 *
 * @param text UTF-8 YAML document text.
 * @return Root `dynamic_ptr` of the parsed document.
 * @throws std::runtime_error on a YAML syntax/parser error.
 */
dynamic_ptr from_yaml(std::string text) {
  yaml_parser_t parser;
  if (!yaml_parser_initialize(&parser)) {
    throw std::runtime_error("Failed to initialize YAML parser");
  }

  yaml_parser_set_input_string(&parser, reinterpret_cast<const unsigned char*>(text.c_str()), text.size());

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

/**
 * @brief Serialize a `dynamic` object to a JSON document string.
 *
 * @param d      Object to serialize.
 * @param keys   Id-to-name lookup used to resolve field keys (see
 *               `resolve_key`); populate with the caller's own literal
 *               field-name strings. Must map each hash back to its exact
 *               original field name so `from_json` can re-derive the same
 *               key on import -- never populate this with `DisplayName`
 *               attribute text.
 * @param indent Passed through to `nlohmann::json::dump`; a negative
 *               value produces compact output, `0` or greater (`2` by
 *               default) produces pretty-printed output with that many
 *               spaces per indent level.
 * @return The JSON document text.
 */
std::string to_json(const dynamic& d, const std::unordered_map<uint32_t, std::string>& keys, int indent) {
  return dynamic_to_json(d, keys).dump(indent);
}

// ─── YAML export helpers
// ──────────────────────────────────────────────────

namespace {

/**
 * @brief Emit a single already-initialized libyaml event, throwing on
 *        failure.
 *
 * Thin wrapper around `yaml_emitter_emit` that converts the emitter's
 * `problem` string into a `std::runtime_error`, used by every other
 * `yaml_*` emit helper below.
 */
static void yaml_send(yaml_emitter_t* e, yaml_event_t& ev) {
  if (!yaml_emitter_emit(e, &ev))
    throw std::runtime_error(std::string("YAML emit: ") + (e->problem ? e->problem : "unknown"));
}

/** @brief Emit @p s as an unquoted (plain-style) YAML scalar. */
static void yaml_scalar_plain(yaml_emitter_t* e, const std::string& s) {
  yaml_event_t ev;
  yaml_scalar_event_initialize(
      &ev,
      nullptr,
      nullptr,
      reinterpret_cast<const yaml_char_t*>(s.c_str()),
      static_cast<int>(s.size()),
      /*plain_implicit=*/1,
      /*quoted_implicit=*/0,
      YAML_PLAIN_SCALAR_STYLE);
  yaml_send(e, ev);
}

/**
 * @brief Emit @p s as a double-quoted YAML scalar.
 *
 * Used for string fields (and the hex encoding of `vector<uint8_t>`) so
 * `yaml_scalar_to_field`'s plain-scalar coercion rules (null/bool/int/
 * float) never misinterpret the value on re-parse.
 */
static void yaml_scalar_quoted(yaml_emitter_t* e, const std::string& s) {
  yaml_event_t ev;
  yaml_scalar_event_initialize(
      &ev,
      nullptr,
      nullptr,
      reinterpret_cast<const yaml_char_t*>(s.c_str()),
      static_cast<int>(s.size()),
      /*plain_implicit=*/0,
      /*quoted_implicit=*/1,
      YAML_DOUBLE_QUOTED_SCALAR_STYLE);
  yaml_send(e, ev);
}

/**
 * @brief Format a `float` as a YAML plain scalar that always parses back
 *        as a float, never an int.
 *
 * Appends `.0` when the default `std::ostringstream` formatting produces
 * no decimal point or exponent (e.g. for whole values like `2.0f`), so
 * `yaml_scalar_to_field`'s int-then-float coercion order can't
 * misclassify the round-tripped value as `int32_t`.
 */
static std::string format_float_yaml(float f) {
  std::ostringstream oss;
  oss << std::setprecision(7) << f;
  std::string s = oss.str();
  if (s.find('.') == std::string::npos && s.find('e') == std::string::npos)
    s += ".0";
  return s;
}

/**
 * @brief Emit a YAML block sequence of plain scalars from a vector field.
 *
 * @tparam Vec Container type (`std::vector<bool>`, `std::vector<int32_t>`,
 *             or `std::vector<float>`).
 * @tparam Fmt Callable converting one element of @p v to its scalar text.
 * @param e   Target emitter.
 * @param v   Elements to emit.
 * @param fmt Per-element formatter.
 */
template <typename Vec, typename Fmt>
static void yaml_emit_sequence(yaml_emitter_t* e, const Vec& v, Fmt fmt) {
  yaml_event_t ev;
  yaml_sequence_start_event_initialize(&ev, nullptr, nullptr, /*implicit=*/1, YAML_BLOCK_SEQUENCE_STYLE);
  yaml_send(e, ev);
  for (const auto& item : v)
    yaml_scalar_plain(e, fmt(item));
  yaml_sequence_end_event_initialize(&ev);
  yaml_send(e, ev);
}

static void emit_field_yaml(yaml_emitter_t* e, const field& f, const key_map& keys);
static void emit_dynamic_yaml(yaml_emitter_t* e, const dynamic& d, const key_map& keys);

/**
 * @brief Emit a single `field`'s YAML representation.
 *
 * Dispatches on the active `field_base` alternative: scalars become plain
 * or quoted scalars as appropriate, `dynamic_ptr` recurses via
 * `emit_dynamic_yaml`, and the vector types are rendered the way
 * described in `yaml_sequence_to_field` (bool/int32/float vectors as
 * plain block sequences, `vector<uint8_t>` as a quoted hex string).
 *
 * @param e    Target emitter.
 * @param f    Field to emit.
 * @param keys Id-to-name lookup used for `hash_t` / `key_t` fields.
 */
static void emit_field_yaml(yaml_emitter_t* e, const field& f, const key_map& keys) {
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

/**
 * @brief Emit a `dynamic` object's named fields as a YAML block mapping.
 *
 * Only named fields are visited (via `dynamic::forEach`); methods and
 * attributes are not exported. Mirrors `dynamic_to_json`'s scope, but
 * always produces a block mapping rather than JSON's flat object.
 * `__class`/`__namespace` fields holding the `0` "unset" sentinel are
 * skipped (see `is_zero_bookkeeping_field_p`).
 *
 * @param e    Target emitter.
 * @param d    Object to emit.
 * @param keys Id-to-name lookup used to resolve field keys.
 */
static void emit_dynamic_yaml(yaml_emitter_t* e, const dynamic& d, const key_map& keys) {
  yaml_event_t ev;
  yaml_mapping_start_event_initialize(&ev, nullptr, nullptr, /*implicit=*/1, YAML_BLOCK_MAPPING_STYLE);
  yaml_send(e, ev);
  d.forEach([&](key_t k, const field& f) {
    if (is_zero_bookkeeping_field_p(k, f))
      return;
    yaml_scalar_plain(e, resolve_key(k.id, keys));
    emit_field_yaml(e, f, keys);
  });
  yaml_mapping_end_event_initialize(&ev);
  yaml_send(e, ev);
}

} // namespace

/**
 * @brief Serialize a `dynamic` object to a YAML document string.
 *
 * Drives a libyaml emitter through a full stream (start/document
 * start/content/document end/stream end) and always emits the object as
 * a block mapping at the document root via `emit_dynamic_yaml`.
 *
 * @param d    Object to serialize.
 * @param keys Id-to-name lookup used to resolve field keys (see
 *             `resolve_key`); populate with the caller's own literal
 *             field-name strings. Must map each hash back to its exact
 *             original field name so `from_yaml` can re-derive the same
 *             key on import -- never populate this with `DisplayName`
 *             attribute text.
 * @return The YAML document text.
 * @throws std::runtime_error on an emitter initialization or emit
 *         failure.
 */
std::string to_yaml(const dynamic& d, const std::unordered_map<uint32_t, std::string>& keys) {
  // Set up emitter with a write callback that appends to a std::string.
  yaml_emitter_t emitter;
  std::string output;
  if (!yaml_emitter_initialize(&emitter))
    throw std::runtime_error("YAML emitter: failed to initialize");

  struct guard_t {
    yaml_emitter_t& e;
    ~guard_t() {
      yaml_emitter_delete(&e);
    }
  } guard{emitter};

  yaml_emitter_set_output(
      &emitter,
      [](void* data, unsigned char* buf, size_t size) -> int {
        static_cast<std::string*>(data)->append(reinterpret_cast<char*>(buf), size);
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

// ─── bison_print ──────────────────────────────────────────────────────────────

} // namespace bdg::bison

#include "src/bison/bison_print.hpp"

#include <iomanip>
#include <sstream>

namespace bdg::bison {

namespace {

/** @brief Quote and backslash-escape @p s for pretty-printed output. */
std::string escape_string_p(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  out += '"';
  for (char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += c;
        break;
    }
  }
  out += '"';
  return out;
}

/** @brief Format @p h as an `#xxxxxxxx` 8-digit lowercase hex hash. */
std::string format_hash_p(hash_t h) {
  std::ostringstream oss;
  oss << '#' << std::hex << std::setw(8) << std::setfill('0') << h;
  return oss.str();
}

/**
 * @brief Format a key for pretty-printed output when no display name is
 *        available.
 *
 * Named keys (high bit set, per the `hash()` scheme in
 * `bison_common.hpp`) are formatted as `#xxxxxxxx` via `format_hash_p`.
 * Numeric-index keys (array-like fields) are formatted as `[N]`.
 */
std::string format_key_p(key_t k) {
  const hash_t h = static_cast<hash_t>(k);
  if (h & 0x80000000u)
    return format_hash_p(h);
  std::ostringstream oss;
  oss << '[' << h << ']';
  return oss.str();
}


/**
 * @brief Format a `float` for pretty-printed output, always with a
 *        trailing decimal point so it reads distinctly from an int.
 */
std::string format_float_p(float f) {
  std::ostringstream oss;
  oss << std::setprecision(7) << f;
  std::string s = oss.str();
  if (s.find('.') == std::string::npos && s.find('e') == std::string::npos)
    s += '.';
  return s;
}

/** @brief Return @p s repeated @p n times, used to build indentation. */
std::string repeat_str_p(const std::string& s, int n) {
  std::string r;
  r.reserve(s.size() * static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    r += s;
  return r;
}

/**
 * @brief Resolve the display label for a field key in pretty-printed
 *        output.
 *
 * Precedence: the field's own `DisplayName` attribute, then the live
 * `_rkey` / `register_key_name` registry (via `lookup_registered_key_name`),
 * then a raw `format_key_p` fallback. The registry is consulted directly
 * (not a caller-supplied snapshot) so a key registered after this object was
 * built is still resolved correctly.
 */
std::string format_field_key_p(key_t k, const field& f) {
  if (const auto* dn = f.findAttribute<DisplayName>())
    return dn->name();
  if (auto s = lookup_registered_key_name(static_cast<hash_t>(k)))
    return *s;
  return format_key_p(k);
}

/** @brief Same precedence as `format_field_key_p`, but for method keys. */
std::string format_method_key_p(key_t k, const method& m) {
  if (const auto* dn = m.findAttribute<DisplayName>())
    return dn->name();
  if (auto s = lookup_registered_key_name(static_cast<hash_t>(k)))
    return *s;
  return format_key_p(k);
}

// Forward declarations for mutual recursion.
std::string print_field_value_p(const field& f, const print_options& opts, int depth);
std::string print_dynamic_p(const dynamic& obj, const print_options& opts, int depth);

/**
 * @brief Render a single `field`'s value for pretty-printed output.
 *
 * Dispatches on the active `field_base` alternative: `hash_t`/`key_t`
 * prefer a name from the live `_rkey` / `register_key_name` registry (via
 * `lookup_registered_key_name`) and fall back to `format_hash_p`,
 * `dynamic_ptr` recurses via `print_dynamic_p`, and vector types are
 * rendered as bracketed, comma-separated element lists (`vector<uint8_t>`
 * is summarized as `<N bytes>` instead of dumping raw bytes).
 *
 * @param f     Field to render.
 * @param opts  Formatting options (indentation, etc).
 * @param depth Current nesting depth, forwarded to nested `dynamic_ptr`
 *              rendering.
 */
std::string print_field_value_p(const field& f, const print_options& opts, int depth) {
  return std::visit(
      [&](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          return "null";
        } else if constexpr (std::is_same_v<T, hash_t>) {
          if (auto s = lookup_registered_key_name(v))
            return *s;
          return format_hash_p(v);
        } else if constexpr (std::is_same_v<T, key_t>) {
          if (auto s = lookup_registered_key_name(static_cast<hash_t>(v)))
            return *s;
          return format_hash_p(static_cast<hash_t>(v));
        } else if constexpr (std::is_same_v<T, bool>) {
          return v ? "true" : "false";
        } else if constexpr (std::is_same_v<T, int32_t>) {
          return std::to_string(v);
        } else if constexpr (std::is_same_v<T, float>) {
          return format_float_p(v);
        } else if constexpr (std::is_same_v<T, dynamic_ptr>) {
          if (!v)
            return "null";
          return print_dynamic_p(*v, opts, depth);
        } else if constexpr (std::is_same_v<T, std::string>) {
          return escape_string_p(v);
        } else if constexpr (std::is_same_v<T, std::vector<bool>>) {
          std::string s = "[";
          bool first = true;
          for (bool b : v) {
            if (!first)
              s += ", ";
            s += b ? "true" : "false";
            first = false;
          }
          return s + "]";
        } else if constexpr (std::is_same_v<T, std::vector<int32_t>>) {
          std::string s = "[";
          bool first = true;
          for (int32_t i : v) {
            if (!first)
              s += ", ";
            s += std::to_string(i);
            first = false;
          }
          return s + "]";
        } else if constexpr (std::is_same_v<T, std::vector<float>>) {
          std::string s = "[";
          bool first = true;
          for (float ff : v) {
            if (!first)
              s += ", ";
            s += format_float_p(ff);
            first = false;
          }
          return s + "]";
        } else {
          static_assert(std::is_same_v<T, std::vector<uint8_t>>);
          std::ostringstream oss;
          oss << '<' << v.size() << " bytes>";
          return oss.str();
        }
      },
      static_cast<const field_base&>(f));
}

/**
 * @brief Render a method's attributes (display name, description,
 *        category, obsolete/required markers) as a compact summary.
 *
 * Returns `"<method>"` when the method has no recognized attributes, or
 * `"<method {key: value, ...}>"` otherwise.
 */
std::string print_method_value_p(const method& m) {
  std::string pairs;
  auto append = [&](const std::string& k, const std::string& val) {
    if (!pairs.empty())
      pairs += ", ";
    pairs += k + ": " + escape_string_p(val);
  };
  if (auto* dn = m.findAttribute<DisplayName>())
    append("displayName", dn->name());
  if (auto* d = m.findAttribute<Description>())
    append("description", d->text());
  if (auto* c = m.findAttribute<Category>())
    append("category", c->name());
  if (auto* o = m.findAttribute<Obsolete>()) {
    if (!pairs.empty())
      pairs += ", ";
    pairs += "obsolete: true";
    if (!o->message().empty())
      append("obsoleteMessage", o->message());
  }
  if (m.findAttribute<Required>()) {
    if (!pairs.empty())
      pairs += ", ";
    pairs += "required: true";
  }
  if (pairs.empty())
    return "<method>";
  return "<method {" + pairs + "}>";
}

/**
 * @brief Render a `dynamic` object's fields and methods as a `{...}`
 *        block, either multiline-indented or single-line per
 *        `opts.multiline`.
 *
 * Internal bookkeeping fields (`__class`, `__parent`, `__namespace`) are
 * skipped when `opts.hide_internal` is set. Independent of that flag,
 * `__class`/`__namespace` holding the `0` "unset" sentinel are always
 * skipped (see `is_zero_bookkeeping_field_p`) since that value never
 * carries identifying information.
 *
 * @param obj   Object to render.
 * @param opts  Formatting options.
 * @param depth Current nesting depth, used for indentation.
 */
std::string print_dynamic_p(const dynamic& obj, const print_options& opts, int depth) {
  std::string result;
  auto is_internal = [](hash_t h) { return h == dynamic::CLASS || h == dynamic::PARENT || h == dynamic::NAMESPACE; };
  auto skip_field = [&](key_t k, const field& f) {
    return (opts.hide_internal && is_internal(static_cast<hash_t>(k))) || is_zero_bookkeeping_field_p(k, f);
  };
  if (opts.multiline) {
    const std::string cur = repeat_str_p(opts.indent, depth);
    const std::string next = repeat_str_p(opts.indent, depth + 1);
    result = "{\n";
    obj.forEach([&](key_t k, const field& f) {
      if (skip_field(k, f))
        return;
      result += next + format_field_key_p(k, f) + ": ";
      result += print_field_value_p(f, opts, depth + 1) + '\n';
    });
    obj.forEachMethod([&](key_t k, const method& m) {
      result += next + format_method_key_p(k, m) + ": ";
      result += print_method_value_p(m) + '\n';
    });
    result += cur + '}';
  } else {
    result = '{';
    bool first = true;
    obj.forEach([&](key_t k, const field& f) {
      if (skip_field(k, f))
        return;
      if (!first)
        result += ", ";
      result += format_field_key_p(k, f) + ": ";
      result += print_field_value_p(f, opts, depth + 1);
      first = false;
    });
    obj.forEachMethod([&](key_t k, const method& m) {
      if (!first)
        result += ", ";
      result += format_method_key_p(k, m) + ": ";
      result += print_method_value_p(m);
      first = false;
    });
    result += '}';
  }
  return result;
}

} // anonymous namespace

/**
 * @brief Render a `dynamic` object as a human-readable debug string.
 *
 * @param obj  Object to render.
 * @param opts Formatting options (dictionary, multiline, indent string,
 *             hide-internal flag).
 * @return The formatted `{...}` block; see `print_dynamic_p`.
 */
std::string print(const dynamic& obj, const print_options& opts) {
  return print_dynamic_p(obj, opts, 0);
}

// ── Key-name registry ─────────────────────────────────────────────────────────

namespace {

/** @brief Process-wide, thread-safe hash → human-readable-name registry. */
synchronized<std::unordered_map<hash_t, std::string>>& key_name_registry() {
  static synchronized<std::unordered_map<hash_t, std::string>> reg;
  return reg;
}

} // namespace

/**
 * @brief Register a human-readable name for a key hash in the global
 *        registry.
 *
 * Called by the `_rkey` literal (see `bison_common.hpp`) and directly by
 * callers who want a name available via `lookup_registered_key_name()`
 * without using the `_rkey` literal itself.
 *
 * @param h    Hash produced by `hash()` or `_key` / `_rkey`.
 * @param name Human-readable name to associate with @p h.
 */
void register_key_name(hash_t h, std::string_view name) {
  key_name_registry().wlock()->emplace(h, std::string(name));
}

std::optional<std::string> lookup_registered_key_name(hash_t h) {
  auto lp = key_name_registry().rlock();
  auto it = lp->find(h);
  if (it == lp->end())
    return std::nullopt;
  return it->second;
}

// ─── bison_flags ──────────────────────────────────────────────────────────────
namespace {

// Placeholder shown after a flag's name in the usage listing, e.g.
// "--port INT32".  Bool flags take no argument, so they get none.
std::string flag_value_placeholder(const gflags::CommandLineFlagInfo& info) {
  if (info.type == "bool")
    return "";
  std::string type = info.type;
  std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return std::toupper(c); });
  return " " + type;
}

std::string program_name(const char* argv0) {
  std::string_view path{argv0};
  auto pos = path.find_last_of("/\\");
  return std::string{pos == std::string_view::npos ? path : path.substr(pos + 1)};
}

} // namespace

bool print_usage(int argc, char** argv, const std::string& description, const char* flags_file) {
  bool help_requested = false;
  for (int i = 1; i < argc; ++i) {
    if (std::string_view{argv[i]} == "--help" || std::string_view{argv[i]} == "-help") {
      help_requested = true;
      break;
    }
  }
  if (!help_requested)
    return false;

  std::vector<gflags::CommandLineFlagInfo> all_flags;
  gflags::GetAllFlags(&all_flags);

  std::vector<gflags::CommandLineFlagInfo> own_flags;
  for (const auto& info : all_flags) {
    if (info.filename == flags_file)
      own_flags.push_back(info);
  }
  std::sort(own_flags.begin(), own_flags.end(), [](const auto& a, const auto& b) { return a.name < b.name; });

  size_t width = 0;
  for (const auto& info : own_flags)
    width = std::max(width, info.name.size() + 2 + flag_value_placeholder(info).size());

  const std::string prog = program_name(argv[0]);
  std::cout << prog << " - " << description << "\n\n"
            << "Usage:\n  " << prog << " [flags]\n\n"
            << "Flags:\n";
  for (const auto& info : own_flags) {
    std::string col = "--" + info.name + flag_value_placeholder(info);
    std::cout << "  " << std::left << std::setw(static_cast<int>(width) + 2) << col << info.description
              << " (default: " << info.default_value << ")\n";
  }
  std::cout << "\nRun with '--helpfull' for the full list of low-level gflags flags.\n";
  return true;
}

} // namespace bdg::bison
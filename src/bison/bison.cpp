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

// ─── bison_print ──────────────────────────────────────────────────────────────

} // namespace bdg::bison

#include "src/bison/bison_print.hpp"

#include <iomanip>
#include <sstream>

namespace bdg::bison {

namespace {

std::string escape_string_p(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  out += '"';
  for (char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:   out += c;      break;
    }
  }
  out += '"';
  return out;
}

std::string format_hash_p(hash_t h) {
  std::ostringstream oss;
  oss << '#' << std::hex << std::setw(8) << std::setfill('0') << h;
  return oss.str();
}

std::string format_key_p(key_t k) {
  const hash_t h = static_cast<hash_t>(k);
  if (h & 0x80000000u) return format_hash_p(h);
  std::ostringstream oss;
  oss << '[' << h << ']';
  return oss.str();
}

const std::string* dict_lookup_p(
    hash_t h, const std::unordered_map<hash_t, std::string>* d) {
  if (!d) return nullptr;
  auto it = d->find(h);
  return it != d->end() ? &it->second : nullptr;
}

std::string format_float_p(float f) {
  std::ostringstream oss;
  oss << std::setprecision(7) << f;
  std::string s = oss.str();
  if (s.find('.') == std::string::npos && s.find('e') == std::string::npos)
    s += '.';
  return s;
}

std::string repeat_str_p(const std::string& s, int n) {
  std::string r;
  r.reserve(s.size() * static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) r += s;
  return r;
}

std::string format_field_key_p(
    key_t k, const field& f, const print_options& opts) {
  if (const auto* dn = f.findAttribute<DisplayName>()) return dn->name();
  if (const auto* s = dict_lookup_p(static_cast<hash_t>(k), opts.dict))
    return *s;
  return format_key_p(k);
}

std::string format_method_key_p(
    key_t k, const method& m, const print_options& opts) {
  if (const auto* dn = m.findAttribute<DisplayName>()) return dn->name();
  if (const auto* s = dict_lookup_p(static_cast<hash_t>(k), opts.dict))
    return *s;
  return format_key_p(k);
}

// Forward declarations for mutual recursion.
std::string print_field_value_p(const field& f, const print_options& opts, int depth);
std::string print_dynamic_p(const dynamic& obj, const print_options& opts, int depth);

std::string print_field_value_p(
    const field& f, const print_options& opts, int depth) {
  return std::visit(
      [&](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          return "null";
        } else if constexpr (std::is_same_v<T, hash_t>) {
          if (const auto* s = dict_lookup_p(v, opts.dict)) return *s;
          return format_hash_p(v);
        } else if constexpr (std::is_same_v<T, key_t>) {
          if (const auto* s = dict_lookup_p(static_cast<hash_t>(v), opts.dict))
            return *s;
          return format_hash_p(static_cast<hash_t>(v));
        } else if constexpr (std::is_same_v<T, bool>) {
          return v ? "true" : "false";
        } else if constexpr (std::is_same_v<T, int32_t>) {
          return std::to_string(v);
        } else if constexpr (std::is_same_v<T, float>) {
          return format_float_p(v);
        } else if constexpr (std::is_same_v<T, dynamic_ptr>) {
          if (!v) return "null";
          return print_dynamic_p(*v, opts, depth);
        } else if constexpr (std::is_same_v<T, std::string>) {
          return escape_string_p(v);
        } else if constexpr (std::is_same_v<T, std::vector<bool>>) {
          std::string s = "[";
          bool first = true;
          for (bool b : v) {
            if (!first) s += ", ";
            s += b ? "true" : "false";
            first = false;
          }
          return s + "]";
        } else if constexpr (std::is_same_v<T, std::vector<int32_t>>) {
          std::string s = "[";
          bool first = true;
          for (int32_t i : v) {
            if (!first) s += ", ";
            s += std::to_string(i);
            first = false;
          }
          return s + "]";
        } else if constexpr (std::is_same_v<T, std::vector<float>>) {
          std::string s = "[";
          bool first = true;
          for (float ff : v) {
            if (!first) s += ", ";
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

std::string print_method_value_p(const method& m) {
  std::string pairs;
  auto append = [&](const std::string& k, const std::string& val) {
    if (!pairs.empty()) pairs += ", ";
    pairs += k + ": " + escape_string_p(val);
  };
  if (auto* dn = m.findAttribute<DisplayName>()) append("displayName", dn->name());
  if (auto* d  = m.findAttribute<Description>()) append("description",  d->text());
  if (auto* c  = m.findAttribute<Category>())    append("category",     c->name());
  if (auto* o  = m.findAttribute<Obsolete>()) {
    if (!pairs.empty()) pairs += ", ";
    pairs += "obsolete: true";
    if (!o->message().empty()) append("obsoleteMessage", o->message());
  }
  if (m.findAttribute<Required>()) {
    if (!pairs.empty()) pairs += ", ";
    pairs += "required: true";
  }
  if (pairs.empty()) return "<method>";
  return "<method {" + pairs + "}>";
}

std::string print_dynamic_p(
    const dynamic& obj, const print_options& opts, int depth) {
  std::string result;
  if (opts.multiline) {
    const std::string cur  = repeat_str_p(opts.indent, depth);
    const std::string next = repeat_str_p(opts.indent, depth + 1);
    result = "{\n";
    obj.forEach([&](key_t k, const field& f) {
      result += next + format_field_key_p(k, f, opts) + ": ";
      result += print_field_value_p(f, opts, depth + 1) + '\n';
    });
    obj.forEachMethod([&](key_t k, const method& m) {
      result += next + format_method_key_p(k, m, opts) + ": ";
      result += print_method_value_p(m) + '\n';
    });
    result += cur + '}';
  } else {
    result = '{';
    bool first = true;
    obj.forEach([&](key_t k, const field& f) {
      if (!first) result += ", ";
      result += format_field_key_p(k, f, opts) + ": ";
      result += print_field_value_p(f, opts, depth + 1);
      first = false;
    });
    obj.forEachMethod([&](key_t k, const method& m) {
      if (!first) result += ", ";
      result += format_method_key_p(k, m, opts) + ": ";
      result += print_method_value_p(m);
      first = false;
    });
    result += '}';
  }
  return result;
}

} // anonymous namespace

std::string print(const dynamic& obj, const print_options& opts) {
  return print_dynamic_p(obj, opts, 0);
}

std::unordered_map<hash_t, std::string> build_display_dict() {
  std::unordered_map<hash_t, std::string> d;
  auto lp = dynamic::getRegistry().rlock();
  for (const auto& [ns_key, classes] : *lp) {
    for (const auto& [klass_key, proto] : classes) {
      if (!proto) continue;
      const auto* cf = proto->findField(dynamic::CLASS);
      if (cf) {
        if (const auto* dn = cf->findAttribute<DisplayName>())
          d[static_cast<hash_t>(klass_key)] = dn->name();
      }
      proto->forEach([&](key_t k, const field& f) {
        if (const auto* dn = f.findAttribute<DisplayName>())
          d[static_cast<hash_t>(k)] = dn->name();
      });
      proto->forEachMethod([&](key_t k, const method& m) {
        if (const auto* dn = m.findAttribute<DisplayName>())
          d[static_cast<hash_t>(k)] = dn->name();
        auto add_params = [&](const dynamic* spec) {
          if (!spec) return;
          spec->forEach([&](key_t fk, const field& ff) {
            if (const auto* dn = ff.findAttribute<DisplayName>())
              d[static_cast<hash_t>(fk)] = dn->name();
          });
        };
        add_params(m.inputSpec());
        add_params(m.outputSpec());
      });
    }
  }
  return d;
}

} // namespace bdg::bison
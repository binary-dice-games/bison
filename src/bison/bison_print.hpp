// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

/**
 * @file bison_print.hpp
 * @brief Human-readable string formatting for `dynamic` objects.
 *
 * Provides `print_options` and the free function `print(dynamic, print_options)`
 * that converts a `dynamic` object to a readable string.  The output is
 * configurable between multiline (indented) and single-line (compact) modes.
 *
 * Field and method keys are displayed using their `DisplayName` attribute when
 * one is attached; otherwise the raw hash (`#xxxxxxxx`) or array index
 * (`[n]`) is used.
 *
 * ## Usage
 * @code{.cpp}
 * #include "src/bison/bison_print.hpp"
 *
 * bison::dynamic obj;
 * obj["name"_key] = bison::field{std::string{"Alice"}, bison::attr<bison::DisplayName>("name")};
 * obj["age"_key]  = bison::field{int32_t{30},          bison::attr<bison::DisplayName>("age")};
 * std::cout << bison::print(obj) << '\n';
 * @endcode
 */

#pragma once

#include "src/bison/bison_object.hpp"

namespace bdg::bison {

/**
 * @brief Options controlling the output format of `print()`.
 */
struct print_options {
  /** @brief Emit one key-value pair per line with indentation when true. */
  bool multiline{true};
  /** @brief String prepended once per nesting level in multiline mode. */
  std::string indent{"  "};
};

namespace detail {

inline std::string escape_string(const std::string& s) {
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

inline std::string format_hash_value(hash_t h) {
  std::ostringstream oss;
  oss << '#' << std::hex << std::setw(8) << std::setfill('0') << h;
  return oss.str();
}

inline std::string format_key(key_t k) {
  const hash_t h = static_cast<hash_t>(k);
  if (h & 0x80000000u)
    return format_hash_value(h);
  std::ostringstream oss;
  oss << '[' << h << ']';
  return oss.str();
}

inline std::string format_float(float f) {
  std::ostringstream oss;
  oss << std::setprecision(7) << f;
  std::string s = oss.str();
  if (s.find('.') == std::string::npos && s.find('e') == std::string::npos)
    s += '.';
  return s;
}

inline std::string repeat_str(const std::string& s, int n) {
  std::string result;
  result.reserve(s.size() * static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    result += s;
  return result;
}

// Forward declarations so print_field_value and print_dynamic can call each other.
std::string print_field_value(const field& f, const print_options& opts, int depth);
std::string print_dynamic(const dynamic& obj, const print_options& opts, int depth);

inline std::string print_field_value(
    const field& f, const print_options& opts, int depth) {
  return std::visit(
      [&](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          return "null";
        } else if constexpr (std::is_same_v<T, hash_t>) {
          return format_hash_value(v);
        } else if constexpr (std::is_same_v<T, key_t>) {
          return format_hash_value(static_cast<hash_t>(v));
        } else if constexpr (std::is_same_v<T, bool>) {
          return v ? "true" : "false";
        } else if constexpr (std::is_same_v<T, int32_t>) {
          return std::to_string(v);
        } else if constexpr (std::is_same_v<T, float>) {
          return format_float(v);
        } else if constexpr (std::is_same_v<T, dynamic_ptr>) {
          if (!v) return "null";
          return print_dynamic(*v, opts, depth);
        } else if constexpr (std::is_same_v<T, std::string>) {
          return escape_string(v);
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
            s += format_float(ff);
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

inline std::string collect_method_attr_pairs(const method& m) {
  std::string s;
  auto append = [&](const std::string& k, const std::string& v) {
    if (!s.empty()) s += ", ";
    s += k + ": " + escape_string(v);
  };
  if (auto* dn = m.findAttribute<DisplayName>()) append("displayName", dn->name());
  if (auto* d  = m.findAttribute<Description>()) append("description", d->text());
  if (auto* c  = m.findAttribute<Category>())    append("category",    c->name());
  if (auto* o  = m.findAttribute<Obsolete>()) {
    if (!s.empty()) s += ", ";
    s += "obsolete: true";
    if (!o->message().empty())
      append("obsoleteMessage", o->message());
  }
  if (m.findAttribute<Required>()) {
    if (!s.empty()) s += ", ";
    s += "required: true";
  }
  return s;
}

inline std::string print_method_value(const method& m) {
  const std::string pairs = collect_method_attr_pairs(m);
  if (pairs.empty()) return "<method>";
  return "<method {" + pairs + "}>";
}

inline std::string format_field_key(key_t k, const field& f) {
  if (const auto* dn = f.findAttribute<DisplayName>())
    return dn->name();
  return format_key(k);
}

inline std::string format_method_key(key_t k, const method& m) {
  if (const auto* dn = m.findAttribute<DisplayName>())
    return dn->name();
  return format_key(k);
}

inline std::string print_dynamic(
    const dynamic& obj, const print_options& opts, int depth) {
  std::string result;
  if (opts.multiline) {
    const std::string cur  = repeat_str(opts.indent, depth);
    const std::string next = repeat_str(opts.indent, depth + 1);
    result = "{\n";
    obj.forEach([&](key_t k, const field& f) {
      result += next;
      result += format_field_key(k, f);
      result += ": ";
      result += print_field_value(f, opts, depth + 1);
      result += '\n';
    });
    obj.forEachMethod([&](key_t k, const method& m) {
      result += next;
      result += format_method_key(k, m);
      result += ": ";
      result += print_method_value(m);
      result += '\n';
    });
    result += cur;
    result += '}';
  } else {
    result = '{';
    bool first = true;
    obj.forEach([&](key_t k, const field& f) {
      if (!first) result += ", ";
      result += format_field_key(k, f);
      result += ": ";
      result += print_field_value(f, opts, depth + 1);
      first = false;
    });
    obj.forEachMethod([&](key_t k, const method& m) {
      if (!first) result += ", ";
      result += format_method_key(k, m);
      result += ": ";
      result += print_method_value(m);
      first = false;
    });
    result += '}';
  }
  return result;
}

} // namespace detail

/**
 * @brief Convert @p obj to a human-readable string.
 *
 * @param obj   Source object.
 * @param opts  Format options; defaults to multiline with two-space indent.
 * @return Formatted string representation of @p obj.
 */
inline std::string print(const dynamic& obj, const print_options& opts = {}) {
  return detail::print_dynamic(obj, opts, 0);
}

} // namespace bdg::bison

// MIT License © 2025 Binary Dice Games
/**
 * @file cli_app.cpp
 * @brief Interactive REPL application scaffold implementation.
 */
#include "src/app/cli/cli_app.hpp"

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/constants.hpp"
#include "src/rmi/transport/socket_transport.hpp"

#if defined(__linux__)
#include "src/app/pty/pty_client_transport.hpp"
#endif

#include <gflags/gflags.h>
#include <nlohmann/json.hpp>

#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

DECLARE_string(host);
DECLARE_int32(port);
DECLARE_bool(pty);
DECLARE_int32(timeout);

namespace bdg::bison::app {

// ── File-local REPL helpers ──────────────────────────────────────────────────

namespace {

// Maps key_t hash IDs to their original string names so the output of
// dynamic_to_json() uses readable field names wherever possible.
using key_name_map = std::unordered_map<uint32_t, std::string>;

// Register a string name so it can be resolved back in output.
static void register_key(key_name_map& km, const std::string& name) {
  if (!name.empty())
    km[bison::key_t{name}.id] = name;
}

// Recursively extract all string field-names from a JSON value and register
// them in the key-name map.  Call this before converting any user-supplied
// JSON to bison::dynamic so that every field key the user typed is resolvable.
static void register_json_keys(key_name_map& km, const nlohmann::json& j) {
  if (j.is_object()) {
    for (const auto& [k, v] : j.items()) {
      register_key(km, k);
      register_json_keys(km, v);
    }
  } else if (j.is_array()) {
    for (const auto& elem : j)
      register_json_keys(km, elem);
  }
}

// Merge a server-side hash→display-name dictionary into the key-name map.
// The dictionary is a flat bison::dynamic where each field key is a hash and
// each field value is the display-name string.
static void merge_dictionary(key_name_map& km, const bison::dynamic& dict) {
  dict.forEach([&](bison::key_t k, const bison::field& f) {
    if (f.is<std::string>())
      km[k.id] = f.as<std::string>();
  });
}

// Build initial map from the well-known RMI protocol field constants.
static key_name_map make_known_keys() {
  using namespace rmi::shared::constants;
  key_name_map m;
  auto add = [&](bison::key_t k, const char* s) { m[k.id] = s; };
  add(FIELD_NAME, "__name");
  add(FIELD_FIELDS, "__fields");
  add(FIELD_METHODS, "__methods");
  add(FIELD_KLASS, "__class");
  add(FIELD_NAMESPACE, "__namespace");
  add(FIELD_PARAMS, "__params");
  add(FIELD_DISPLAY_NAME, "__displayName");
  add(FIELD_DESCRIPTION, "__description");
  add(FIELD_CATEGORY, "__category");
  add(FIELD_OBSOLETE, "__obsolete");
  add(FIELD_OBSOLETE_MESSAGE, "__obsoleteMessage");
  add(FIELD_REQUIRED, "__required");
  add(FIELD_ERROR, "__error");
  add(FIELD_ERROR_CODE, "__code");
  add(FIELD_ERROR_MESSAGE, "__message");
  return m;
}

// ── String utilities ─────────────────────────────────────────────────────────

static std::string trim(std::string_view s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string_view::npos)
    return {};
  size_t b = s.find_last_not_of(" \t\r\n");
  return std::string(s.substr(a, b - a + 1));
}

// Return the content between the first '(' at open_pos and its matching ')'.
// Returns empty string if parens are unmatched.
static std::string extract_parens(std::string_view s, size_t open_pos) {
  int depth = 1;
  bool in_str = false;
  bool escape = false;
  size_t start = open_pos + 1;
  for (size_t i = start; i < s.size(); ++i) {
    char c = s[i];
    if (escape) {
      escape = false;
      continue;
    }
    if (in_str) {
      if (c == '\\')
        escape = true;
      else if (c == '"')
        in_str = false;
      continue;
    }
    if (c == '"') {
      in_str = true;
      continue;
    }
    if (c == '(' || c == '{' || c == '[')
      ++depth;
    else if (c == ')' || c == '}' || c == ']') {
      --depth;
      if (depth == 0)
        return std::string(s.substr(start, i - start));
    }
  }
  return {};
}

// Split an argument list by commas at depth 0, respecting nested {} [] () "".
static std::vector<std::string> split_args(std::string_view s) {
  std::vector<std::string> result;
  int depth = 0;
  bool in_str = false;
  bool escape = false;
  size_t start = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (escape) {
      escape = false;
      continue;
    }
    if (in_str) {
      if (c == '\\')
        escape = true;
      else if (c == '"')
        in_str = false;
      continue;
    }
    if (c == '"') {
      in_str = true;
      continue;
    }
    if (c == '{' || c == '[' || c == '(')
      ++depth;
    else if (c == '}' || c == ']' || c == ')') {
      if (depth > 0)
        --depth;
    } else if (c == ',' && depth == 0) {
      auto a = trim(s.substr(start, i - start));
      if (!a.empty())
        result.push_back(a);
      start = i + 1;
    }
  }
  auto last = trim(s.substr(start));
  if (!last.empty())
    result.push_back(last);
  return result;
}

// Unescape a JSON-quoted string literal (e.g. "\"Ikea\"" → "Ikea").
// Falls back to stripping the outer quotes if JSON parsing fails.
static std::string unquote(std::string_view s) {
  if (s.size() < 2 || s.front() != '"' || s.back() != '"')
    return std::string(s);
  try {
    return nlohmann::json::parse(s).get<std::string>();
  } catch (...) {
    return std::string(s.substr(1, s.size() - 2));
  }
}

// ── dynamic → JSON ───────────────────────────────────────────────────────────

static nlohmann::json dynamic_to_json(
    const bison::dynamic& d,
    const key_name_map& km);

static nlohmann::json field_to_json(
    const bison::field& f,
    const key_name_map& km) {
  return std::visit(
      [&](const auto& v) -> nlohmann::json {
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
        } else if constexpr (std::is_same_v<T, bison::hash_t>) {
          auto it = km.find(v);
          if (it != km.end())
            return it->second;
          return std::string("#") + std::to_string(v);
        } else if constexpr (std::is_same_v<T, bison::key_t>) {
          auto it = km.find(v.id);
          if (it != km.end())
            return it->second;
          return std::string("#") + std::to_string(v.id);
        } else if constexpr (std::is_same_v<T, bison::dynamic_ptr>) {
          if (v)
            return dynamic_to_json(*v, km);
          return nullptr;
        } else if constexpr (std::is_same_v<T, std::vector<bool>>) {
          auto arr = nlohmann::json::array();
          for (bool b : v)
            arr.push_back(b);
          return arr;
        } else if constexpr (
            std::is_same_v<T, std::vector<int32_t>> ||
            std::is_same_v<T, std::vector<float>>) {
          return nlohmann::json(v);
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
      static_cast<const bison::field_base&>(f));
}

static nlohmann::json dynamic_to_json(
    const bison::dynamic& d,
    const key_name_map& km) {
  auto obj = nlohmann::json::object();
  d.forEach([&](bison::key_t k, const bison::field& f) {
    auto it = km.find(k.id);
    std::string key_str = (it != km.end())
        ? it->second
        : (std::string("#") + std::to_string(k.id));
    obj[key_str] = field_to_json(f, km);
  });
  return obj;
}

// ── Future helper
// ─────────────────────────────────────────────────────────────

template <typename T>
static T get_future(std::future<T> fut, std::chrono::milliseconds timeout) {
  auto res = fut.wait_for(timeout);
  if (res != std::future_status::ready && res != std::future_status::deferred)
    throw std::runtime_error("request timed out");
  return fut.get();
}

// ── Argument conversion
// ───────────────────────────────────────────────────────

// Parse a raw arg string to a bison::dynamic.
// String literals (e.g. "Ikea") must be passed through unquote() separately.
static bison::dynamic parse_json_arg(const std::string& raw) {
  auto ptr = bison::extensions::from_json(raw);
  return ptr ? *ptr : bison::dynamic{};
}

// ── Help text
// ─────────────────────────────────────────────────────────────────

static const char* const k_help_text = R"(Commands:
  name = instantiate("namespace", "Class")      create an instance
  name = instantiate("namespace", "Class", {})  create with params
  name.get()                                    get all fields (JSON)
  name.get({"field": null, ...})                get projected fields
  name.set({"field": value, ...})               set fields
  name.call("method", {})                       call a method
  del name                                      destroy an instance
  describe                                      list all server classes
  describe("namespace", "Class")                describe one class
  info                                          server help and class listing
  list                                          show active instances
  help                                          this message
  exit  |  quit  |  Ctrl+D                      disconnect and exit)";

// ── REPL dispatcher
// ───────────────────────────────────────────────────────────

// Returns false when the REPL should stop.
static bool dispatch(
    const std::string& line,
    rmi::client& c,
    std::unordered_map<std::string, rmi::proxy::dynamic>& handles,
    key_name_map& km,
    std::chrono::milliseconds timeout) {
  auto s = trim(line);
  if (s.empty() || s.front() == '#')
    return true;

  if (s == "exit" || s == "quit")
    return false;

  if (s == "help") {
    std::cout << k_help_text << '\n';
    return true;
  }

  if (s == "list") {
    if (handles.empty()) {
      std::cout << "(no active instances)\n";
    } else {
      for (const auto& [name, proxy] : handles)
        std::cout << name << "  (id=" << proxy.id() << ")\n";
    }
    return true;
  }

  // del <name>
  if (s.size() > 4 && s.substr(0, 4) == "del ") {
    auto var = trim(s.substr(4));
    auto it = handles.find(var);
    if (it == handles.end()) {
      std::cerr << "error: '" << var << "' is not defined\n";
    } else {
      auto node = handles.extract(it);
      c.destroy(std::move(node.mapped()));
    }
    return true;
  }

  // Find the first '(' in the line, if any.
  size_t first_paren = std::string::npos;
  {
    bool in_str = false, esc = false;
    for (size_t i = 0; i < s.size(); ++i) {
      if (esc) {
        esc = false;
        continue;
      }
      if (in_str) {
        if (s[i] == '\\')
          esc = true;
        else if (s[i] == '"')
          in_str = false;
        continue;
      }
      if (s[i] == '"') {
        in_str = true;
        continue;
      }
      if (s[i] == '(') {
        first_paren = i;
        break;
      }
    }
  }

  // Detect assignment: first '=' that appears before any '('.
  size_t eq_pos = std::string::npos;
  for (size_t i = 0; i < s.size(); ++i) {
    if (first_paren != std::string::npos && i >= first_paren)
      break;
    if (s[i] == '=') {
      eq_pos = i;
      break;
    }
  }

  if (eq_pos != std::string::npos) {
    // name = instantiate("Ns", "Class"[, {params}])
    auto var = trim(s.substr(0, eq_pos));
    auto rhs = trim(s.substr(eq_pos + 1));

    size_t rhs_paren = rhs.find('(');
    if (rhs_paren == std::string::npos) {
      std::cerr << "error: expected function call on right-hand side\n";
      return true;
    }
    auto fn = trim(rhs.substr(0, rhs_paren));

    if (fn != "instantiate") {
      std::cerr << "error: only 'instantiate(...)' is supported here\n";
      return true;
    }

    auto args_str = extract_parens(rhs, rhs_paren);
    auto args = split_args(args_str);
    if (args.size() < 2) {
      std::cerr << "error: instantiate requires namespace and class name\n";
      return true;
    }

    auto ns_str = unquote(trim(args[0]));
    auto class_str = unquote(trim(args[1]));
    register_key(km, ns_str);
    register_key(km, class_str);

    bison::key_t ns_key =
        ns_str.empty() ? bison::key_t{0u} : bison::key_t{ns_str};
    bison::key_t class_key = bison::key_t{class_str};

    bison::dynamic params;
    if (args.size() >= 3) {
      auto raw = trim(args[2]);
      try {
        register_json_keys(km, nlohmann::json::parse(raw));
      } catch (...) {
      }
      params = parse_json_arg(raw);
    }

    try {
      auto proxy = get_future(
          c.instantiate(ns_key, class_key, std::move(params)), timeout);
      handles.try_emplace(var, std::move(proxy));
      std::cout << var << '\n';
    } catch (const std::exception& ex) {
      std::cerr << "error: " << ex.what() << '\n';
    }
    return true;
  }

  // Detect method call: <name>.<op>(...) — '.' before first '('.
  if (first_paren != std::string::npos) {
    size_t dot_pos = std::string::npos;
    for (size_t i = 0; i < first_paren; ++i) {
      if (s[i] == '.') {
        dot_pos = i;
        break;
      }
    }

    if (dot_pos != std::string::npos) {
      auto obj_name = trim(s.substr(0, dot_pos));
      auto op = trim(s.substr(dot_pos + 1, first_paren - dot_pos - 1));
      auto args_str = extract_parens(s, first_paren);
      auto args = split_args(args_str);

      auto it = handles.find(obj_name);
      if (it == handles.end()) {
        std::cerr << "error: '" << obj_name << "' is not defined\n";
        return true;
      }
      auto& proxy = it->second;

      if (op == "get") {
        try {
          bison::dynamic result;
          if (args.empty()) {
            result = get_future(proxy.get(), timeout);
          } else {
            auto raw = trim(args[0]);
            try {
              register_json_keys(km, nlohmann::json::parse(raw));
            } catch (...) {
            }
            result = get_future(proxy.get(parse_json_arg(raw)), timeout);
          }
          std::cout << dynamic_to_json(result, km).dump(2) << '\n';
        } catch (const std::exception& ex) {
          std::cerr << "error: " << ex.what() << '\n';
        }
      } else if (op == "set") {
        if (args.empty()) {
          std::cerr << "error: set requires a JSON object argument\n";
          return true;
        }
        try {
          auto raw = trim(args[0]);
          try {
            register_json_keys(km, nlohmann::json::parse(raw));
          } catch (...) {
          }
          get_future(proxy.set(parse_json_arg(raw)), timeout);
        } catch (const std::exception& ex) {
          std::cerr << "error: " << ex.what() << '\n';
        }
      } else if (op == "call") {
        if (args.empty()) {
          std::cerr << "error: call requires a method name\n";
          return true;
        }
        auto method_name = unquote(trim(args[0]));
        register_key(km, method_name);
        bison::dynamic params;
        if (args.size() >= 2) {
          auto raw = trim(args[1]);
          try {
            register_json_keys(km, nlohmann::json::parse(raw));
          } catch (...) {
          }
          params = parse_json_arg(raw);
        }
        try {
          auto result = get_future(
              proxy.call(bison::key_t{method_name}, std::move(params)),
              timeout);
          std::cout << dynamic_to_json(result, km).dump(2) << '\n';
        } catch (const std::exception& ex) {
          std::cerr << "error: " << ex.what() << '\n';
        }
      } else {
        std::cerr << "error: unknown operation '" << op
                  << "' (available: get, set, call)\n";
      }
      return true;
    }

    // Command with parens: describe("Ns", "Class")
    auto cmd = trim(s.substr(0, first_paren));
    auto args_str = extract_parens(s, first_paren);
    auto args = split_args(args_str);

    if (cmd == "describe") {
      bison::key_t ns_key{0u};
      bison::key_t class_key{0u};
      if (args.size() >= 1) {
        auto ns_str = unquote(trim(args[0]));
        register_key(km, ns_str);
        ns_key = bison::key_t{ns_str};
      }
      if (args.size() >= 2) {
        auto class_str = unquote(trim(args[1]));
        register_key(km, class_str);
        class_key = bison::key_t{class_str};
      }
      try {
        auto result = get_future(c.describe(ns_key, class_key), timeout);
        std::cout << dynamic_to_json(result, km).dump(2) << '\n';
      } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << '\n';
      }
    } else {
      std::cerr << "error: unknown command '" << cmd
                << "' (type 'help' for available commands)\n";
    }
    return true;
  }

  // Bare command: describe
  if (s == "describe") {
    try {
      auto result = get_future(c.describe(0u, 0u), timeout);
      std::cout << dynamic_to_json(result, km).dump(2) << '\n';
    } catch (const std::exception& ex) {
      std::cerr << "error: " << ex.what() << '\n';
    }
    return true;
  }

  // Bare command: info
  if (s == "info") {
    try {
      using namespace rmi::shared::constants;
      auto result = get_future(c.get_help(), timeout);
      const auto* f = result.findField(FIELD_DESCRIPTION);
      if (f && f->is<std::string>())
        std::cout << f->as<std::string>();
      else
        std::cout << dynamic_to_json(result, km).dump(2) << '\n';
    } catch (const std::exception& ex) {
      std::cerr << "error: " << ex.what() << '\n';
    }
    return true;
  }

  std::cerr << "error: unknown command '" << s
            << "' (type 'help' for available commands)\n";
  return true;
}

} // namespace

// ── cli_app default hook implementations ─────────────────────────────────────

void cli_app::on_connected() const {}

void cli_app::on_connect_params(bison::dynamic& params) const {
  params["timeout_ms"_key] = int32_t{30000};
}

void cli_app::on_error(const std::string& msg) const {
  std::cerr << "[cli_app] error: " << msg << '\n';
}

// ── cli_app::on_session — default REPL ───────────────────────────────────────

int cli_app::on_session(rmi::client& c) {
  std::unordered_map<std::string, rmi::proxy::dynamic> handles;
  key_name_map km = make_known_keys();
  std::string line;

  std::cout << "bison-cli connected. Type 'help' for commands.\n";

  // Seed the key-name map with display names from the server.
  try {
    auto dict = get_future(c.get_dictionary(), timeout_);
    merge_dictionary(km, dict);
  } catch (...) {
    // Non-fatal: server may not support OP_DICTIONARY.
  }

  while (true) {
    std::cout << "> " << std::flush;
    if (!std::getline(std::cin, line))
      break; // EOF / Ctrl+D
    if (!dispatch(line, c, handles, km, timeout_))
      break;
  }

  // Drain all remaining proxies before disconnecting.
  for (auto& [name, proxy] : handles)
    c.destroy(std::move(proxy));
  handles.clear();

  return 0;
}

// ── cli_app::run — argument parsing and lifecycle
// ─────────────────────────────

int cli_app::run(int argc, char** argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_pty && (FLAGS_host != "127.0.0.1" || FLAGS_port != 7070)) {
    on_error("--pty cannot be combined with --host or --port");
    return 1;
  }

  timeout_ = std::chrono::milliseconds{FLAGS_timeout};

  try {
    std::unique_ptr<rmi::transport::client_transport_iface> transport;

    if (FLAGS_pty) {
#if defined(__linux__)
      transport = std::make_unique<pty_client_transport>();
#else
      on_error("--pty is only supported on Linux");
      return 1;
#endif
    } else {
      transport = std::make_unique<rmi::transport::socket_client_transport>(
          FLAGS_host, static_cast<uint16_t>(FLAGS_port));
    }

    rmi::client c{std::move(transport)};

    bison::dynamic params;
    on_connect_params(params);
    c.connect(std::move(params));
    on_connected();

    const int result = on_session(c);
    c.disconnect();
    return result;

  } catch (const std::exception& ex) {
    on_error(ex.what());
    return 1;
  } catch (...) {
    on_error("unexpected failure");
    return 1;
  }
}

} // namespace bdg::bison::app

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

#include <iostream>
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

// ── Types ─────────────────────────────────────────────────────────────────────

/// Maps key_t hash IDs back to their original string names for readable output.
using key_name_map = std::unordered_map<uint32_t, std::string>;

/// All mutable state shared across REPL command handlers within one session.
struct repl_context {
  rmi::client& client;
  std::unordered_map<std::string, rmi::proxy::dynamic>& handles;
  key_name_map& km;
  std::chrono::milliseconds timeout;
};

// ── Key name registry ─────────────────────────────────────────────────────────

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

// ── String utilities ──────────────────────────────────────────────────────────

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
    if (escape) { escape = false; continue; }
    if (in_str) {
      if (c == '\\') escape = true;
      else if (c == '"') in_str = false;
      continue;
    }
    if (c == '"') { in_str = true; continue; }
    if (c == '(' || c == '{' || c == '[') ++depth;
    else if (c == ')' || c == '}' || c == ']') {
      if (--depth == 0)
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
    if (escape) { escape = false; continue; }
    if (in_str) {
      if (c == '\\') escape = true;
      else if (c == '"') in_str = false;
      continue;
    }
    if (c == '"') { in_str = true; continue; }
    if (c == '{' || c == '[' || c == '(') ++depth;
    else if (c == '}' || c == ']' || c == ')') { if (depth > 0) --depth; }
    else if (c == ',' && depth == 0) {
      auto a = trim(s.substr(start, i - start));
      if (!a.empty()) result.push_back(a);
      start = i + 1;
    }
  }
  auto last = trim(s.substr(start));
  if (!last.empty()) result.push_back(last);
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


// ── Future helper ──────────────────────────────────────────────────────────────

template <typename T>
static T get_future(std::future<T> fut, std::chrono::milliseconds timeout) {
  auto res = fut.wait_for(timeout);
  if (res != std::future_status::ready && res != std::future_status::deferred)
    throw std::runtime_error("request timed out");
  return fut.get();
}

// ── Argument conversion ────────────────────────────────────────────────────────

static bison::dynamic parse_json_arg(const std::string& raw) {
  auto ptr = bison::extensions::from_json(raw);
  return ptr ? *ptr : bison::dynamic{};
}

// Parse @p raw as JSON, register all its string keys into ctx.km, then return
// the resulting bison::dynamic.  JSON parse errors are silently ignored so that
// non-JSON raw values (e.g. bare strings) are still attempted via parse_json_arg.
static bison::dynamic parse_and_register(
    repl_context& ctx, const std::string& raw) {
  try {
    register_json_keys(ctx.km, nlohmann::json::parse(raw));
  } catch (...) {}
  return parse_json_arg(raw);
}

// ── Help text ─────────────────────────────────────────────────────────────────

static constexpr const char* k_help_text = R"(Commands:
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

// ── Command handlers ──────────────────────────────────────────────────────────

/**
 * @brief Handle `list` — print all active variable names and their object IDs.
 */
static void cmd_list(const repl_context& ctx) {
  if (ctx.handles.empty()) {
    std::cout << "(no active instances)\n";
  } else {
    for (const auto& [name, proxy] : ctx.handles)
      std::cout << name << "  (id=" << proxy.id() << ")\n";
  }
}

/**
 * @brief Handle `del <name>` — destroy the named proxy and remove it from the
 *        variable table.
 *
 * @param var  Variable name to destroy (may have leading/trailing whitespace).
 */
static void cmd_del(repl_context& ctx, std::string_view var) {
  const auto name = trim(var);
  auto it = ctx.handles.find(name);
  if (it == ctx.handles.end()) {
    std::cerr << "error: '" << name << "' is not defined\n";
    return;
  }
  auto node = ctx.handles.extract(it);
  ctx.client.destroy(std::move(node.mapped()));
}

/**
 * @brief Handle `name = instantiate("Ns", "Class"[, {params}])`.
 *
 * Creates a server-side object, stores the resulting proxy under @p var in
 * the handle table, and prints @p var on success.
 *
 * @param var       Variable name to assign the proxy to.
 * @param args_str  Raw comma-separated argument string extracted from the parens.
 */
static void cmd_instantiate(
    repl_context& ctx,
    const std::string& var,
    const std::string& args_str) {
  const auto args = split_args(args_str);
  if (args.size() < 2) {
    std::cerr << "error: instantiate requires namespace and class name\n";
    return;
  }

  const auto ns_str = unquote(trim(args[0]));
  const auto class_str = unquote(trim(args[1]));
  register_key(ctx.km, ns_str);
  register_key(ctx.km, class_str);

  bison::key_t ns_key = ns_str.empty() ? bison::key_t{0u} : bison::key_t{ns_str};
  bison::key_t class_key = bison::key_t{class_str};

  bison::dynamic params;
  if (args.size() >= 3)
    params = parse_and_register(ctx, trim(args[2]));

  try {
    auto proxy = get_future(
        ctx.client.instantiate(ns_key, class_key, std::move(params)),
        ctx.timeout);
    ctx.handles.try_emplace(var, std::move(proxy));
    std::cout << var << '\n';
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << '\n';
  }
}

/**
 * @brief Handle `name.get([projection])` — retrieve fields from a remote object.
 *
 * Without arguments retrieves all fields.  With a JSON-object projection
 * retrieves only the listed keys.  Prints the result as pretty-printed JSON.
 *
 * @param proxy  Target proxy.
 * @param args   Parsed argument list; first element (if present) is a JSON
 *               projection object, e.g. `{"field": null}`.
 */
static void cmd_get(
    repl_context& ctx,
    rmi::proxy::dynamic& proxy,
    const std::vector<std::string>& args) {
  try {
    bison::dynamic result = args.empty()
        ? get_future(proxy.get(), ctx.timeout)
        : get_future(
              proxy.get(parse_and_register(ctx, trim(args[0]))), ctx.timeout);
    std::cout << bison::extensions::to_json(result, ctx.km) << '\n';
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << '\n';
  }
}

/**
 * @brief Handle `name.set({fields})` — apply a partial field update.
 *
 * @param proxy  Target proxy.
 * @param args   Parsed argument list; first element must be a JSON object of
 *               field-name/value pairs to apply.
 */
static void cmd_set(
    repl_context& ctx,
    rmi::proxy::dynamic& proxy,
    const std::vector<std::string>& args) {
  if (args.empty()) {
    std::cerr << "error: set requires a JSON object argument\n";
    return;
  }
  try {
    get_future(proxy.set(parse_and_register(ctx, trim(args[0]))), ctx.timeout);
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << '\n';
  }
}

/**
 * @brief Handle `name.call("method"[, {params}])` — invoke a remote method.
 *
 * Prints the return value as pretty-printed JSON.
 *
 * @param proxy  Target proxy.
 * @param args   Parsed argument list: args[0] is the method name (quoted
 *               string); args[1] (optional) is a JSON parameters object.
 */
static void cmd_call(
    repl_context& ctx,
    rmi::proxy::dynamic& proxy,
    const std::vector<std::string>& args) {
  if (args.empty()) {
    std::cerr << "error: call requires a method name\n";
    return;
  }
  const auto method_name = unquote(trim(args[0]));
  register_key(ctx.km, method_name);

  bison::dynamic params;
  if (args.size() >= 2)
    params = parse_and_register(ctx, trim(args[1]));

  try {
    auto result = get_future(
        proxy.call(bison::key_t{method_name}, std::move(params)), ctx.timeout);
    std::cout << bison::extensions::to_json(result, ctx.km) << '\n';
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << '\n';
  }
}

/**
 * @brief Handle `describe[("Ns", "Class")]` — list server classes or describe one.
 *
 * Without arguments queries all namespaces and classes.  With arguments prints
 * the field and method schema for the named class.
 *
 * @param args  Empty to list all classes; otherwise args[0]=namespace,
 *              args[1]=class.
 */
static void cmd_describe(
    repl_context& ctx, const std::vector<std::string>& args) {
  bison::key_t ns_key{0u};
  bison::key_t class_key{0u};
  if (args.size() >= 1) {
    const auto ns_str = unquote(trim(args[0]));
    register_key(ctx.km, ns_str);
    ns_key = bison::key_t{ns_str};
  }
  if (args.size() >= 2) {
    const auto class_str = unquote(trim(args[1]));
    register_key(ctx.km, class_str);
    class_key = bison::key_t{class_str};
  }
  try {
    auto result = get_future(ctx.client.describe(ns_key, class_key), ctx.timeout);
    std::cout << bison::extensions::to_json(result, ctx.km) << '\n';
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << '\n';
  }
}

/**
 * @brief Handle `info` — print human-readable server help text from OP_HELP.
 *
 * Prints the `__description` string field from the response when present;
 * otherwise falls back to pretty-printed JSON of the full response payload.
 */
static void cmd_info(repl_context& ctx) {
  try {
    using namespace rmi::shared::constants;
    auto result = get_future(ctx.client.get_help(), ctx.timeout);
    const auto* f = result.findField(FIELD_DESCRIPTION);
    if (f && f->is<std::string>())
      std::cout << f->as<std::string>();
    else
      std::cout << bison::extensions::to_json(result, ctx.km) << '\n';
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << '\n';
  }
}

// ── REPL line parser helpers ──────────────────────────────────────────────────

/// Return the index of the first '(' not inside a string literal, or npos.
static size_t find_first_paren(std::string_view s) {
  bool in_str = false, esc = false;
  for (size_t i = 0; i < s.size(); ++i) {
    if (esc) { esc = false; continue; }
    if (in_str) {
      if (s[i] == '\\') esc = true;
      else if (s[i] == '"') in_str = false;
      continue;
    }
    if (s[i] == '"') { in_str = true; continue; }
    if (s[i] == '(') return i;
  }
  return std::string::npos;
}

// ── REPL line-form handlers ───────────────────────────────────────────────────

/**
 * @brief Handle `name = instantiate("Ns", "Class"[, {params}])`.
 *
 * Validates that the right-hand side is an `instantiate(...)` call, then
 * delegates to `cmd_instantiate`.
 *
 * @param s    Trimmed input line.
 * @param ctx  Active REPL context.
 * @param eq_pos  Position of the '=' separator in @p s.
 */
static void dispatch_assignment(
    const std::string& s, repl_context& ctx, size_t eq_pos) {
  const auto var = trim(s.substr(0, eq_pos));
  const auto rhs = trim(s.substr(eq_pos + 1));
  const size_t rhs_paren = rhs.find('(');
  if (rhs_paren == std::string::npos) {
    std::cerr << "error: expected function call on right-hand side\n";
    return;
  }
  const auto fn = trim(rhs.substr(0, rhs_paren));
  if (fn != "instantiate") {
    std::cerr << "error: only 'instantiate(...)' is supported here\n";
    return;
  }
  cmd_instantiate(ctx, var, extract_parens(rhs, rhs_paren));
}

/**
 * @brief Handle `name.op(...)` — proxy operation in dot notation.
 *
 * Looks up the object name in the handle table and dispatches to
 * `cmd_get`, `cmd_set`, or `cmd_call` based on @p op.
 *
 * @param s            Trimmed input line.
 * @param ctx          Active REPL context.
 * @param dot_pos      Position of the '.' in @p s.
 * @param first_paren  Position of the first '(' in @p s.
 */
static void dispatch_proxy_op(
    const std::string& s, repl_context& ctx,
    size_t dot_pos, size_t first_paren) {
  const auto obj_name = trim(s.substr(0, dot_pos));
  const auto op       = trim(s.substr(dot_pos + 1, first_paren - dot_pos - 1));
  const auto args     = split_args(extract_parens(s, first_paren));

  auto it = ctx.handles.find(obj_name);
  if (it == ctx.handles.end()) {
    std::cerr << "error: '" << obj_name << "' is not defined\n";
    return;
  }
  auto& proxy = it->second;

  if      (op == "get")  cmd_get(ctx, proxy, args);
  else if (op == "set")  cmd_set(ctx, proxy, args);
  else if (op == "call") cmd_call(ctx, proxy, args);
  else
    std::cerr << "error: unknown operation '" << op
              << "' (available: get, set, call)\n";
}

/**
 * @brief Handle a command followed by parenthesised arguments, e.g.
 *        `describe("Ns", "Class")`.
 *
 * @param s            Trimmed input line.
 * @param ctx          Active REPL context.
 * @param first_paren  Position of the opening '(' in @p s.
 */
static void dispatch_paren_cmd(
    const std::string& s, repl_context& ctx, size_t first_paren) {
  const auto cmd  = trim(s.substr(0, first_paren));
  const auto args = split_args(extract_parens(s, first_paren));
  if (cmd == "describe")
    cmd_describe(ctx, args);
  else
    std::cerr << "error: unknown command '" << cmd
              << "' (type 'help' for available commands)\n";
}

// ── REPL dispatcher ───────────────────────────────────────────────────────────

/// Parse and dispatch one REPL line.  Returns false when the session should end.
static bool dispatch(const std::string& line, repl_context& ctx) {
  const auto s = trim(line);
  if (s.empty() || s.front() == '#')
    return true;
  if (s == "exit" || s == "quit")
    return false;

  // Bare keyword commands.
  if (s == "help")     { std::cout << k_help_text << '\n'; return true; }
  if (s == "list")     { cmd_list(ctx);         return true; }
  if (s == "describe") { cmd_describe(ctx, {}); return true; }
  if (s == "info")     { cmd_info(ctx);         return true; }

  // del <name>
  if (s.size() > 4 && s.substr(0, 4) == "del ") {
    cmd_del(ctx, s.substr(4));
    return true;
  }

  const size_t first_paren = find_first_paren(s);

  // Find the first '=' before any '(' to detect assignment.
  const size_t eq_limit = (first_paren != std::string::npos) ? first_paren : s.size();
  for (size_t i = 0; i < eq_limit; ++i) {
    if (s[i] == '=') {
      dispatch_assignment(s, ctx, i);
      return true;
    }
  }

  if (first_paren != std::string::npos) {
    const size_t dot_pos = s.find('.');
    if (dot_pos < first_paren)
      dispatch_proxy_op(s, ctx, dot_pos, first_paren);
    else
      dispatch_paren_cmd(s, ctx, first_paren);
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

  std::cout << "bison-cli connected. Type 'help' for commands.\n";

  // Seed the key-name map with display names from the server.
  try {
    auto dict = get_future(c.get_dictionary(), timeout_);
    merge_dictionary(km, dict);
  } catch (...) {
    // Non-fatal: server may not support OP_DICTIONARY.
  }

  repl_context ctx{c, handles, km, timeout_};
  std::string line;
  while (true) {
    std::cout << "> " << std::flush;
    if (!std::getline(std::cin, line))
      break; // EOF / Ctrl+D
    if (!dispatch(line, ctx))
      break;
  }

  // Drain all remaining proxies before disconnecting.
  for (auto& [name, proxy] : handles)
    c.destroy(std::move(proxy));

  return 0;
}

// ── cli_app::run — flag validation and lifecycle ───────────────────────────────

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

// MIT License © 2025 Binary Dice Games
/**
 * @file cli_app.cpp
 * @brief Interactive REPL client implementation.
 *
 * Contains only `cli_app::on_session()` — the interactive REPL.  Transport
 * selection, flag parsing, and the connect/disconnect lifecycle live in
 * `client_app::run()` (src/app/client/client_app.cpp).
 */
#include "src/app/cli/cli_app.hpp"

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/constants.hpp"

#include <nlohmann/json.hpp>

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bdg::bison::app {

// ── File-local REPL helpers ───────────────────────────────────────────────────

namespace {

using key_name_map = std::unordered_map<uint32_t, std::string>;

struct repl_context {
  rmi::client& client;
  std::unordered_map<std::string, rmi::proxy::dynamic>& handles;
  key_name_map& km;
  std::chrono::milliseconds timeout;
};

static void register_key(key_name_map& km, const std::string& name) {
  if (!name.empty())
    km[bison::key_t{name}.id] = name;
}

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

static void merge_dictionary(key_name_map& km, const bison::dynamic& dict) {
  dict.forEach([&](bison::key_t k, const bison::field& f) {
    if (f.is<std::string>())
      km[k.id] = f.as<std::string>();
  });
}

static key_name_map make_known_keys() {
  using namespace rmi::shared::constants;
  key_name_map m;
  auto add = [&](bison::key_t k, const char* s) { m[k.id] = s; };
  add(FIELD_NAME,             "__name");
  add(FIELD_FIELDS,           "__fields");
  add(FIELD_METHODS,          "__methods");
  add(FIELD_KLASS,            "__class");
  add(FIELD_NAMESPACE,        "__namespace");
  add(FIELD_PARAMS,           "__params");
  add(FIELD_DISPLAY_NAME,     "__displayName");
  add(FIELD_DESCRIPTION,      "__description");
  add(FIELD_CATEGORY,         "__category");
  add(FIELD_OBSOLETE,         "__obsolete");
  add(FIELD_OBSOLETE_MESSAGE, "__obsoleteMessage");
  add(FIELD_REQUIRED,         "__required");
  add(FIELD_ERROR,            "__error");
  add(FIELD_ERROR_CODE,       "__code");
  add(FIELD_ERROR_MESSAGE,    "__message");
  return m;
}

static std::string trim(std::string_view s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string_view::npos) return {};
  size_t b = s.find_last_not_of(" \t\r\n");
  return std::string(s.substr(a, b - a + 1));
}

static std::string extract_parens(std::string_view s, size_t open_pos) {
  int depth = 1;
  bool in_str = false, escape = false;
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

static std::vector<std::string> split_args(std::string_view s) {
  std::vector<std::string> result;
  int depth = 0;
  bool in_str = false, escape = false;
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

static std::string unquote(std::string_view s) {
  if (s.size() < 2 || s.front() != '"' || s.back() != '"')
    return std::string(s);
  try {
    return nlohmann::json::parse(s).get<std::string>();
  } catch (...) {
    return std::string(s.substr(1, s.size() - 2));
  }
}

template <typename T>
static T get_future(std::future<T> fut, std::chrono::milliseconds timeout) {
  auto res = fut.wait_for(timeout);
  if (res != std::future_status::ready && res != std::future_status::deferred)
    throw std::runtime_error("request timed out");
  return fut.get();
}

static bison::dynamic parse_json_arg(const std::string& raw) {
  auto ptr = bison::extensions::from_json(raw);
  return ptr ? *ptr : bison::dynamic{};
}

static bison::dynamic parse_and_register(
    repl_context& ctx, const std::string& raw) {
  try {
    register_json_keys(ctx.km, nlohmann::json::parse(raw));
  } catch (...) {}
  return parse_json_arg(raw);
}

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

static void cmd_list(const repl_context& ctx) {
  if (ctx.handles.empty()) {
    std::cout << "(no active instances)\n";
  } else {
    for (const auto& [name, proxy] : ctx.handles)
      std::cout << name << "  (id=" << proxy.id() << ")\n";
  }
}

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

static void cmd_instantiate(
    repl_context& ctx,
    const std::string& var,
    const std::string& args_str) {
  const auto args = split_args(args_str);
  if (args.size() < 2) {
    std::cerr << "error: instantiate requires namespace and class name\n";
    return;
  }
  const auto ns_str    = unquote(trim(args[0]));
  const auto class_str = unquote(trim(args[1]));
  register_key(ctx.km, ns_str);
  register_key(ctx.km, class_str);

  bison::key_t ns_key    = ns_str.empty() ? bison::key_t{0u} : bison::key_t{ns_str};
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

static void cmd_describe(
    repl_context& ctx, const std::vector<std::string>& args) {
  bison::key_t ns_key{0u}, class_key{0u};
  if (!args.empty()) {
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

static bool dispatch(const std::string& line, repl_context& ctx) {
  const auto s = trim(line);
  if (s.empty() || s.front() == '#') return true;
  if (s == "exit" || s == "quit")    return false;

  if (s == "help")     { std::cout << k_help_text << '\n'; return true; }
  if (s == "list")     { cmd_list(ctx);         return true; }
  if (s == "describe") { cmd_describe(ctx, {}); return true; }
  if (s == "info")     { cmd_info(ctx);         return true; }

  if (s.size() > 4 && s.substr(0, 4) == "del ") {
    cmd_del(ctx, s.substr(4));
    return true;
  }

  const size_t first_paren = find_first_paren(s);
  const size_t eq_limit    = (first_paren != std::string::npos) ? first_paren : s.size();
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

// ── cli_app::on_error ─────────────────────────────────────────────────────────

void cli_app::on_error(const std::string& msg) const {
  std::cerr << "[cli_app] error: " << msg << '\n';
}

// ── cli_app::on_session — interactive REPL ────────────────────────────────────

int cli_app::on_session(rmi::client& c) {
  std::unordered_map<std::string, rmi::proxy::dynamic> handles;
  key_name_map km = make_known_keys();

  std::cout << "bison-cli connected. Type 'help' for commands.\n";

  try {
    auto dict = get_future(c.get_dictionary(), timeout_);
    merge_dictionary(km, dict);
  } catch (...) {}

  repl_context ctx{c, handles, km, timeout_};
  std::string line;
  while (true) {
    std::cout << "> " << std::flush;
    if (!std::getline(std::cin, line)) break;
    if (!dispatch(line, ctx))          break;
  }

  for (auto& [name, proxy] : handles)
    c.destroy(std::move(proxy));

  return 0;
}

} // namespace bdg::bison::app

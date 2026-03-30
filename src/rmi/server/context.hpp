// MIT License © 2025 Binary Dice Games
#pragma once

#include "src/core/bison.hpp"
#include "src/rmi/shared/ids.hpp"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace bdg::bison::rmi {

struct context {
  std::string session_id;
  std::unordered_map<std::string, std::shared_ptr<bison::dynamic>> objects;
  std::function<void(const std::string&, bison::key_t, bison::dynamic)> emit_event;
};

} // namespace bdg::bison::rmi

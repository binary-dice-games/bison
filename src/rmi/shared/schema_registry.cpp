// MIT License © 2025 Binary Dice Games
/**
 * @file schema_registry.cpp
 * @brief Global storage and dispatch for RMI schema registrations.
 */
#include "src/rmi/shared/schema_registry.hpp"

#include <mutex>
#include <unordered_set>
#include <vector>

namespace bdg::bison::rmi::shared::schema_registry {
namespace {

std::mutex registry_mutex;
std::vector<schema_registration_fn> registrations;
std::unordered_set<schema_registration_fn> registration_set;

} // namespace

void add_schema_registration(schema_registration_fn fn) {
  if (fn == nullptr)
    return;

  std::lock_guard<std::mutex> lk(registry_mutex);
  if (registration_set.insert(fn).second) {
    registrations.push_back(fn);
  }
}

void register_all_schemas() {
  std::vector<schema_registration_fn> snapshot;
  {
    std::lock_guard<std::mutex> lk(registry_mutex);
    snapshot = registrations;
  }

  for (auto fn : snapshot) {
    fn();
  }
}

} // namespace bdg::bison::rmi::shared::schema_registry

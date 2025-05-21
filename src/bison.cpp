#include <bison.hpp>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace bdg {
namespace bison {

std::shared_ptr<dynamic> from_json_array(json::array_t data);
std::shared_ptr<dynamic> from_json_object(json::object_t data);

std::shared_ptr<dynamic> from_json_array(json::array_t data) {
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

std::shared_ptr<dynamic> from_json_object(json::object_t data) {
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

namespace extensions {
std::shared_ptr<dynamic> from_json(std::string text) {
  json data = json::parse(text);
  return from_json_object(data);
}

} // namespace extensions
} // namespace bison
} // namespace bdg
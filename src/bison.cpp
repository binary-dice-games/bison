#include "scone.hpp"

namespace bdg {
namespace bison {

node::node(key_t klass, std::map<key_t, field> &&fields)
  : fields_(std::move(fields))
{
  fields_["__class"_key] = klass;
}

node::~node() {}

node node::clone() const
{
  return node(static_cast<const node>(*this));
}

size_t node::size() const
{
  auto it = fields_.rbegin();
  return it != fields_.rend() && it->first >= 0 ? (size_t)(it->first + 1) : 0;
}

bool node::erase(size_t pos)
{
  return fields_.erase(static_cast<hash_t>(pos)) != 0;
}

void node::clear()
{
  fields_.erase(fields_.lower_bound(0), fields_.end());
}

field &node::operator[](size_t pos)
{
  return fields_[static_cast<hash_t>(pos)];
}

const field &node::operator[](size_t pos) const
{
  return fields_[static_cast<hash_t>(pos)];
}

field &node::operator[](key_t name)
{
  auto field = findField(name);
  return field != nullptr ? *field : fields_[name];
}

const field &node::operator[](key_t name) const
{
  auto field = findField(name);
  return field != nullptr ? *field : fields_[name];
}

bool node::addField(key_t name, field value)
{
  return fields_.emplace(std::make_pair(name, std::move(value))).second;
}

bool node::addMethod(key_t name, method fn)
{
  return methods_.emplace(std::make_pair(name, fn)).second;
}

node node::call(key_t name, const node &params)
{
  auto fn = findMethod(name);
  if (fn == nullptr) {
    throw std::runtime_error("Method not found");
  }
  return (*fn)(*this, params);
}

bool node::addClass(const key_t parent, node &&klass)
{
  std::unique_lock<std::mutex> lk(getMutex());
  auto name = klass.as<key_t>("__class"_key);
  klass["__parent"_key] = parent;

  auto ancestor = parent;
  auto &classes = getClasses();
  auto it = classes.find(parent);
  while (it != classes.end() && ancestor != name) {
    ancestor = it->second.as<key_t>("__parent"_key);
    it = classes.find(ancestor);
  }

  if (ancestor == name) {
    return false;
  }

  return classes.try_emplace(name, std::move(klass)).second;
}

field *node::findField(key_t name) const
{
  auto it = fields_.find(name);
  if (it == fields_.end()) {
    std::unique_lock<std::mutex> lk(getMutex());
    auto &classes = getClasses();
    auto itClass = classes.find(as<key_t>("__parent"_key));
    while (itClass != classes.end() && it == fields_.end()) {
      auto &klass = itClass->second;
      auto itField = klass.fields_.find(name);
      if (itField != klass.fields_.end()) {
        it = fields_.insert(std::make_pair(name, itField->second)).first;
      } else {
        itClass = classes.find(klass.as<key_t>("__parent"_key));
      }
    }
  }

  return it != fields_.end() ? &it->second : nullptr;
}

method *node::findMethod(key_t name) const
{
  auto it = methods_.find(name);
  if (it == methods_.end()) {
    std::unique_lock<std::mutex> lk(getMutex());
    auto &classes = getClasses();
    auto itClass = classes.find(as<key_t>("__parent"_key));
    while (itClass != classes.end() && it == methods_.end()) {
      auto &klass = itClass->second;
      auto itMethod = klass.methods_.find(name);
      if (itMethod != klass.methods_.end()) {
        it = methods_.insert(std::make_pair(name, itMethod->second)).first;
      } else {
        itClass = classes.find(klass.as<key_t>("__parent"_key));
      }
    }
  }

  return it != methods_.end() ? &it->second : nullptr;
}

node *node::findClass(key_t name) const
{
  std::unique_lock<std::mutex> lk(getMutex());
  auto &classes = getClasses();
  auto klass = as<key_t>("__class"_key);
  auto it = classes.find(klass);
  while (it != classes.end() && klass != name) {
    klass = it->second.as<key_t>("__parent"_key);
    it = classes.find(klass);
  }

  return it != classes.end() ? &it->second : nullptr;
}

}
}
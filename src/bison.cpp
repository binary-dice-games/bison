#include "scone.hpp"

namespace bdg {
namespace bison {

std::atomic<int32_t> node::watchdog_{0};

std::unordered_map<hash_t, node, hash_t, hash_t> node::classes_{};

node::node(hash_t klass, std::map<hash_t, field> &&fields)
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
  return it != fields_.rend() && it->first >= 0 ? (size_t)(it->first + 1): 0;
}

bool node::erase(size_t pos)
{
  return fields_.erase(static_cast<key_t>(pos)) != 0;
}

field &node::operator[](size_t pos)
{
  return fields_[static_cast<key_t>(pos)];
}

const field &node::operator[](size_t pos) const
{
  return fields_[static_cast<key_t>(pos)];
}

field &node::operator[](hash_t name)
{
  return fields_[name];
}

const field &node::operator[](hash_t name) const
{
  return fields_[name];
}

bool node::addMethod(hash_t name, method fn)
{
  return false;
}

node node::call(hash_t name, const node &params)
{
  return node{0};
}

bool node::addClass(const hash_t name, const hash_t parent, node &&klass)
{
  int32_t value = 0;
  while (!watchdog_.compare_exchange_weak(value, -1)) {
  }

  classes_.try_emplace(name, std::move(klass));
  return true;
}

//node make_node(std::initializer_list<std::pair<hash_t, field>> initList)
//{
//  node node;
//  for (const auto &pair : initList) {
//    auto test = field(pair.second);
//    node[pair.first] = std::move(test);
//  }
//  return node;
//}

}
}
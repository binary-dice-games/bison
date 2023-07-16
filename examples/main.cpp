// tests.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include "src/bison.hpp"

using namespace bdg::bison;

class Test {
  public:
  Test() {}
  Test(const Test &that) = delete;
  Test(Test &&that) noexcept = default;
  Test &operator=(const Test &that) = delete;

  mutable std::map<int, std::variant<std::monostate, int, Test>> nodes_;
};

int main()
{
  // std::pair<int, Test> mypair{1, Test()};
  // std::cout << mypair.first;
  std::map<int, Test> mymap{};
  auto &x = mymap[0].nodes_[0];
  //std::variant<std::monostate, int, Test> val;
  //x = val;

  //node a{};
  //auto b = std::move(a);

  std::cout << "Hello World!\n";

  node::addClass("apartment"_key, node{"room"_key});
  node::addClass("floor"_key, node{"apartment"_key});
  node::addClass("building"_key, node{"floor"_key});
  node::addClass("room"_key, node{"building"_key});

  //std::map<hash_t, field> othermap{{"a"_key, {}}};

  node node{"test"_key, {}};
  node["a"_key] = 12;
  int64_t value = node.as<int32_t>("a"_key);

  auto size = node.size();
  node[3] = false;
  size = node.size();
  //node[0] = false;
  node[1] = false;
  node[2] = false;
  node[5] = 14;
  size = node.size();
  node[8] = std::string("test");
  size = node.size();
  node.clear();

  node.erase(5);
  size = node.size();
  node.erase(4);
  size = node.size();
  node.erase(8);
  size = node.size();
  node["child"_key] = bdg::bison::node();

  //auto onemore = make_node({{"a"_key, field(1)}});

  bdg::bison::node node2{
    "parent"_key,
    {{"a"_key, 1},
     {"b"_key, std::string("dos")},
     {"c"_key,
      bdg::bison::node{"child"_key, {{"a"_key, std::string("tres")}}}}}};

  node.as<std::string>("pos_x"_key) = "test";
  //node.as<bool>("pos_x"_key) = false;

  auto clone = node.clone();

  auto type = std::holds_alternative<std::string>(node["pos_x"_key]);

  auto &val1 = node.as<std::string>("pos_x"_key);
  val1 = "other";

  auto &val2 = node.as<std::string>("pos_x"_key);

  node["pos_y"_key] = 12;
  auto val3 = std::get<int32_t>(node["pos_y"_key]);

  return 0;
}

// Run program: Ctrl + F5 or Debug > Start Without Debugging menu
// Debug program: F5 or Debug > Start Debugging menu

// Tips for Getting Started:
//   1. Use the Solution Explorer window to add/manage files
//   2. Use the Team Explorer window to connect to source control
//   3. Use the Output window to see build output and other messages
//   4. Use the Error List window to view errors
//   5. Go to Project > Add New Item to create new code files, or Project > Add Existing Item to add existing code files to the project
//   6. In the future, to open this project again, go to File > Open > Project and select the .sln file

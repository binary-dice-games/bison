// tests.cpp : This file contains the 'main' function. Program execution begins
// and ends there.
//

#include <iostream>
#include <sstream>
#include "src/bison.hpp"

using namespace bdg::bison;

class Test {
 public:
  Test() {}
  Test(const Test& that) = delete;
  Test(Test&& that) noexcept = default;
  Test& operator=(const Test& that) = delete;

  mutable std::map<int, std::variant<std::monostate, int, Test>> dynamics_;
};

int main() {

  auto swap = byte_swap(int32_t{0x01020304});

  // std::pair<int, Test> mypair{1, Test()};
  // std::cout << mypair.first;
  std::map<int, Test> mymap{};
  auto& x = mymap[0].dynamics_[0];
  // std::variant<std::monostate, int, Test> val;
  // x = val;

  // dynamic a{};
  // auto b = std::move(a);

  std::cout << "Hello World!\n";

  auto json = R"({
    "a": "b",
    "c": {
      "d": "e",
      "f": "g"
    }
  })";

  auto json_value = extensions::from_json(json);

  std::stringstream ss;
  dynamic_ptr target;
  dynamic_ptr source{"test"_key, {{"a"_key, true}}};
  (*source)["list"_key] = std::vector<int32_t>{1, 2, 3, 4};
  source->serialize(serializer(ss));
  target = dynamic::deserialize(deserializer(ss));

  (*source)["other"_key] = 1234;

  dynamic::addClass("apartment"_key, dynamic_ptr{"room"_key});
  dynamic::addClass("floor"_key, dynamic_ptr{"apartment"_key});
  dynamic::addClass("building"_key, dynamic_ptr{"floor"_key});
  dynamic::addClass("room"_key, dynamic_ptr{"building"_key});

  return 0;
}

// Run program: Ctrl + F5 or Debug > Start Without Debugging menu
// Debug program: F5 or Debug > Start Debugging menu

// Tips for Getting Started:
//   1. Use the Solution Explorer window to add/manage files
//   2. Use the Team Explorer window to connect to source control
//   3. Use the Output window to see build output and other messages
//   4. Use the Error List window to view errors
//   5. Go to Project > Add New Item to create new code files, or Project > Add
//   Existing Item to add existing code files to the project
//   6. In the future, to open this project again, go to File > Open > Project
//   and select the .sln file

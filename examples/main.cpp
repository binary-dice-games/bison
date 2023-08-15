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

  dynamic_ptr values{"test"_key, {{"a"_key, true}}};
  std::stringstream ss;
  values.serialize(ss);
  values.deserialize(ss);

  dynamic::addClass("apartment"_key, dynamic_ptr{"room"_key});
  dynamic::addClass("floor"_key, dynamic_ptr{"apartment"_key});
  dynamic::addClass("building"_key, dynamic_ptr{"floor"_key});
  dynamic::addClass("room"_key, dynamic_ptr{"building"_key});

  // std::map<hash_t, field> othermap{{"a"_key, {}}};

  dynamic_ptr dynamic{"test"_key, {}};
  dynamic["a"_key] = 12;
  int64_t value = dynamic->as<int32_t>("a"_key);
  auto value2 = 2 * dynamic["a"_key].as<int32_t>();
  auto value3 = 2 * (int32_t)(dynamic["a"_key]);
  // std::string value4 = dynamic["a"_key];

  auto size = dynamic->size();
  dynamic[3] = false;
  size = dynamic->size();
  // dynamic[0] = false;
  dynamic[1] = false;
  dynamic[2] = false;
  dynamic[5] = 14;
  size = dynamic->size();
  dynamic[8] = std::string("test");
  size = dynamic->size();
  dynamic->clear();

  dynamic->erase(5);
  size = dynamic->size();
  dynamic->erase(4);
  size = dynamic->size();
  dynamic->erase(8);
  size = dynamic->size();
  // dynamic["child"_key] = make_dynamic(
  //     ""_key,
  //     {{"a"_key, 1},
  //      {"b"_key, std::string("dos")},
  //      {"c"_key, make_dynamic(""_key, {})}});
  dynamic["child"_key] = dynamic_ptr{"class"_key, {{"example"_key, "example"}}};

  // auto onemore = make_dynamic({{"a"_key, field(1)}});

  bdg::bison::dynamic_ptr dynamic2{
      "parent"_key, {{"a"_key, 1}, {"b"_key, std::string("dos")}}};

  dynamic->as<std::string>("pos_x"_key) = "test";
  // dynamic.as<bool>("pos_x"_key) = false;

  auto clone = dynamic->clone();

  auto type = std::holds_alternative<std::string>(dynamic["pos_x"_key]);

  auto& val1 = dynamic->as<std::string>("pos_x"_key);
  val1 = "other";

  auto& val2 = dynamic->as<std::string>("pos_x"_key);

  dynamic["pos_y"_key] = 12;
  auto val3 = std::get<int32_t>(dynamic["pos_y"_key]);

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

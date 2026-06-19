// MIT License © 2025 Binary Dice Games
/**
 * @file calc_server.cpp
 * @brief Calculator class registration for the calc-server application.
 */
#include "src/srv/calc/calc_server.hpp"

#include "src/bison/bison_object.hpp"

namespace bdg::bison::srv {

void calc_server::register_classes() {
  using namespace bison;

  // ── Prototype ─────────────────────────────────────────────────────────────

  auto proto = dynamic_ptr{"Calculator"_key};

  // User-accessible accumulator field.
  proto->addField(
      "memory"_key,
      field{0.0f,
            attr<DisplayName>("Memory"),
            attr<Description>("User-accessible accumulator value")});

  // ── Methods ───────────────────────────────────────────────────────────────

  proto->addMethod(
      "add"_key,
      method{
          [](dynamic& /*self*/, const dynamic& params) -> dynamic {
            float a = params["a"_key];
            float b = params["b"_key];
            dynamic result;
            result["result"_key] = a + b;
            return result;
          },
          {attr<DisplayName>("Add"),
           attr<Description>("Return a + b")}});

  proto->addMethod(
      "subtract"_key,
      method{
          [](dynamic& /*self*/, const dynamic& params) -> dynamic {
            float a = params["a"_key];
            float b = params["b"_key];
            dynamic result;
            result["result"_key] = a - b;
            return result;
          },
          {attr<DisplayName>("Subtract"),
           attr<Description>("Return a - b")}});

  proto->addMethod(
      "multiply"_key,
      method{
          [](dynamic& /*self*/, const dynamic& params) -> dynamic {
            float a = params["a"_key];
            float b = params["b"_key];
            dynamic result;
            result["result"_key] = a * b;
            return result;
          },
          {attr<DisplayName>("Multiply"),
           attr<Description>("Return a * b")}});

  proto->addMethod(
      "divide"_key,
      method{
          [](dynamic& /*self*/, const dynamic& params) -> dynamic {
            float a = params["a"_key];
            float b = params["b"_key];
            dynamic result;
            if (b == 0.0f) {
              result["error"_key]  = std::string{"division by zero"};
              result["result"_key] = 0.0f;
            } else {
              result["result"_key] = a / b;
            }
            return result;
          },
          {attr<DisplayName>("Divide"),
           attr<Description>("Return a / b; sets error key on division by zero")}});

  // ── Register in global namespace ──────────────────────────────────────────

  dynamic::addClass(
      0U, proto, 0U,
      {attr<DisplayName>("Calculator"),
       attr<Description>("Stateful calculator with a memory field")});
}

} // namespace bdg::bison::srv

// MIT License © 2025 Binary Dice Games
/**
 * @file calc_server.hpp
 * @brief Calculator RMI server application.
 */
#pragma once

#include "src/app/server/server_app.hpp"

namespace bdg::bison::srv {

/**
 * @brief Server that exposes a stateful Calculator class over RMI.
 *
 * Registers the `Calculator` class in the global namespace.  The class
 * exposes one instance field:
 *
 * | Field    | Type  | Default | Description          |
 * |----------|-------|---------|----------------------|
 * | `memory` | float | 0.0     | User-accessible accumulator |
 *
 * And four arithmetic methods:
 *
 * | Method     | Params      | Returns           |
 * |------------|-------------|-------------------|
 * | `add`      | `a`, `b`    | `{"result": a+b}` |
 * | `subtract` | `a`, `b`    | `{"result": a-b}` |
 * | `multiply` | `a`, `b`    | `{"result": a*b}` |
 * | `divide`   | `a`, `b`    | `{"result": a/b}` (error key on b==0) |
 *
 * Example CLI session:
 * @code
 * > c = instantiate("", "Calculator")
 * > c.get()
 * { "memory": 0.0 }
 * > c.call("add", {"a": 10.0, "b": 3.0})
 * { "result": 13.0 }
 * > c.set({"memory": 42.0})
 * > c.get()
 * { "memory": 42.0 }
 * @endcode
 */
class calc_server : public app::server_app {
 public:
  std::string server_description() const override {
    return "Stateful arithmetic calculator server.\n"
           "Exposes a Calculator class with a persistent memory field and\n"
           "four arithmetic operations: Add, Subtract, Multiply, Divide.";
  }

 protected:
  void register_classes() override;
};

} // namespace bdg::bison::srv

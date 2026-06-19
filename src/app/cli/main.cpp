// MIT License © 2025 Binary Dice Games
/**
 * @file main.cpp
 * @brief Entry point for the standalone bison-cli interactive REPL.
 */
#include "src/app/cli/cli_app.hpp"

int main(int argc, char** argv) {
  bdg::bison::app::cli_app app;
  return app.run(argc, argv);
}

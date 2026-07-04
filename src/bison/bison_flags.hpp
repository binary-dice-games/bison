// MIT License © 2025 Binary Dice Games
/**
 * @file bison_flags.hpp
 * @brief Readable `--help` output for gflags-based command-line tools.
 */
#pragma once

#include <string>

namespace bdg::bison {

/**
 * @brief Print a readable usage screen if `--help`/`-help` is present in
 *        `argv`, and report whether it did.
 *
 * Enumerates the gflags flags `DEFINE_*`'d in `flags_file` (pass `__FILE__`
 * from the caller's own `main.cpp`) via gflags' reflection API, so the
 * listing stays in sync with whatever flags exist there instead of being
 * hand-maintained. Flags defined elsewhere (gflags' own built-ins, other
 * translation units) are omitted; `--helpfull` still shows the exhaustive
 * gflags dump for those.
 *
 * Call this before `gflags::ParseCommandLineFlags()` and return immediately
 * if it returns true -- the flags have not been parsed yet at that point.
 *
 * @param argc        Argument count from `main`.
 * @param argv        Argument vector from `main`.
 * @param description Short description of the tool, printed under its name.
 * @param flags_file  Source file whose `DEFINE_*`'d flags should be listed;
 *                    pass `__FILE__` from the caller.
 * @return true if usage was printed (caller should exit), false otherwise.
 */
bool print_usage(int argc, char** argv, const std::string& description, const char* flags_file);

} // namespace bdg::bison

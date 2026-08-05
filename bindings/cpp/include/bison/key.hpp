// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

/**
 * @file key.hpp
 * @brief `hash_t` / `key_t` name-hashing types for the header-only C++ ABI
 *        binding.
 *
 * Mirrors `src/bison/bison_common.hpp`'s hashing system byte-for-byte (same
 * FNV-1a algorithm, same MSB-set convention) so that a `key_t` computed here
 * always matches the hash the precompiled `bison_abi` library computes for
 * the same name via `bison_key()`. Unlike `bison_key()` -- a runtime call
 * across the ABI boundary -- `hash()` here is `constexpr`, so `"name"_key`
 * folds to a compile-time constant with zero runtime cost, exactly like the
 * internal `"name"_key` literal.
 *
 * This header intentionally does not include `bison_c.h`: it has no
 * dependency on the ABI at all, so it is usable standalone.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace bdg::bison::abi {

/** @brief 32-bit hash type used as the wire representation of a name. */
using hash_t = uint32_t;

/**
 * @brief Compute a compile-time FNV-1a hash of a null-terminated string.
 *
 * The MSB of the result is always set so that hashed names are
 * distinguishable from plain numeric indices (small non-negative integers
 * stored in the same key space). Identical algorithm to
 * `bdg::bison::hash()` in `src/bison/bison_common.hpp`, reproduced here so
 * this binding has no dependency on the internal headers.
 *
 * @param input  Null-terminated ASCII/UTF-8 string to hash.
 * @return FNV-1a hash with the MSB forced to 1.
 */
constexpr hash_t hash(const char* input) {
  hash_t value = sizeof(hash_t) == 8 ? 0xcbf29ce484222325 : 0x811c9dc5;
  hash_t mask = sizeof(hash_t) == 8 ? 0x8000000000000000 : 0x80000000;
  const hash_t prime = sizeof(hash_t) == 8 ? 0x00000100000001b3 : 0x01000193;

  while (*input) {
    value ^= static_cast<hash_t>(*input);
    value *= prime;
    ++input;
  }

  return value | mask;
}

/**
 * @brief Key type that wraps a `hash_t`.
 *
 * Supports construction from a raw `hash_t`, a `const char*` (hashed at
 * compile time when the argument is a constant expression), and a
 * `std::string` (hashed at runtime). Distinguishing `key_t` from a plain
 * `int32_t` at the C++ type level is what lets `dynamic::operator[]`
 * dispatch a `bison::key_t`-valued field write to `bison_set_key()` instead
 * of `bison_set_int()` purely via overload resolution -- see
 * `dynamic.hpp`'s `field_ref::operator=`.
 */
struct key_t {
  constexpr key_t(hash_t v = 0) : id(v) {}
  constexpr key_t(const char* input) : id(hash(input)) {}
  key_t(const std::string& input) : id(hash(input.c_str())) {}
  constexpr operator hash_t() const {
    return id;
  }
  hash_t id;
};

/**
 * @brief User-defined literal that hashes a string constant to a `key_t` at
 *        compile time.
 *
 * Enables concise field/method-name hashing identical in spelling and
 * behavior to the internal `"name"_key` literal, e.g. `obj["score"_key]`.
 *
 * @param name  Null-terminated string literal.
 * @param size  Length of the literal (unused; provided by the compiler).
 * @return `key_t` wrapping the FNV-1a hash of @p name with MSB set.
 */
constexpr key_t operator""_key(const char* name, std::size_t size) noexcept {
  (void)size;
  return key_t{hash(name)};
}

} // namespace bdg::bison::abi

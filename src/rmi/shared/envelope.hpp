// MIT License © 2025 Binary Dice Games
#pragma once

#include "src/core/bison.hpp"
#include "src/rmi/shared/constants.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace bdg::bison::rmi::shared {

class rmi_error : public std::runtime_error {
 public:
  rmi_error(bison::key_t code, std::string message);
  bison::key_t code() const;
 private:
  bison::key_t code_;
};

void register_envelope();

std::string encode_payload(const bison::dynamic& payload);
std::shared_ptr<bison::dynamic> decode_payload(const std::string& bytes);
std::string encode_error(bison::key_t code, const std::string& message);

std::vector<char> encode_envelope(
    bison::key_t kind,
    bison::key_t op,
    const std::string& request_id,
    const std::string& object_id,
    bool oneway,
    const std::string& payload,
    const std::string& error = {});

std::shared_ptr<bison::dynamic> decode_envelope(const std::vector<char>& bytes);

} // namespace bdg::bison::rmi::shared

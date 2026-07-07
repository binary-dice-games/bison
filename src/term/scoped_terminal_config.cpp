// MIT License © 2025 Binary Dice Games
/**
 * @file scoped_terminal_config.cpp
 * @brief Constructor/destructor plus the platform-independent CRLF
 *        streambuf translation. The raw-mode half (`impl`,
 *        create_state/release_state) is implemented per platform in
 *        scoped_terminal_config_posix.cpp / _win.cpp.
 */
#include "src/term/scoped_terminal_config.hpp"

#include <iostream>
#include <streambuf>
#include <string>

namespace bdg::bison::term {

namespace {

/** @brief Streambuf that rewrites '\n' -> "\r\n" and forwards each write to a sink. */
class crlf_streambuf : public std::streambuf {
 public:
  explicit crlf_streambuf(std::function<void(std::string_view)> sink) : sink_(std::move(sink)) {}

 protected:
  std::streamsize xsputn(const char* s, std::streamsize n) override {
    translated_.clear();
    translated_.reserve(static_cast<size_t>(n) + 8);
    for (std::streamsize i = 0; i < n; ++i) {
      if (s[i] == '\n')
        translated_.push_back('\r');
      translated_.push_back(s[i]);
    }
    sink_(translated_);
    return n;
  }

  int_type overflow(int_type ch) override {
    if (traits_type::eq_int_type(ch, traits_type::eof()))
      return traits_type::not_eof(ch);
    const char c = traits_type::to_char_type(ch);
    xsputn(&c, 1);
    return ch;
  }

 private:
  std::function<void(std::string_view)> sink_;
  std::string translated_;
};

} // namespace

/** @brief Saved cout/cerr streambufs plus the filters installed in their place. */
struct scoped_terminal_config::crlf_state {
  crlf_streambuf cout_filter;
  crlf_streambuf cerr_filter;
  std::streambuf* saved_cout;
  std::streambuf* saved_cerr;

  crlf_state(std::function<void(std::string_view)> cout_sink, std::function<void(std::string_view)> cerr_sink)
      : cout_filter(std::move(cout_sink)),
        cerr_filter(std::move(cerr_sink)),
        saved_cout(std::cout.rdbuf(&cout_filter)),
        saved_cerr(std::cerr.rdbuf(&cerr_filter)) {}
};

scoped_terminal_config::crlf_state* scoped_terminal_config::create_crlf_state(const params& p) {
  if (p.sink) {
    return new crlf_state(p.sink, p.sink);
  }
  auto* orig_cout = std::cout.rdbuf();
  auto* orig_cerr = std::cerr.rdbuf();
  return new crlf_state(
      [orig_cout](std::string_view s) { orig_cout->sputn(s.data(), static_cast<std::streamsize>(s.size())); },
      [orig_cerr](std::string_view s) { orig_cerr->sputn(s.data(), static_cast<std::streamsize>(s.size())); });
}

void scoped_terminal_config::release_crlf_state(crlf_state* state) {
  std::cout.rdbuf(state->saved_cout);
  std::cerr.rdbuf(state->saved_cerr);
  delete state;
}

scoped_terminal_config::scoped_terminal_config(params&& p)
    : params_(std::move(p)), impl_(create_state(params_)), crlf_(create_crlf_state(params_)) {}

scoped_terminal_config::~scoped_terminal_config() {
  release_crlf_state(crlf_);
  release_state(impl_);
}

} // namespace bdg::bison::term

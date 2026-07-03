// MIT License © 2025 Binary Dice Games
/**
 * @file crlf_output_guard.cpp
 * @brief Implementation of crlf_output_guard. See the header for the
 *        rationale.
 */
#include "src/pty/crlf_output_guard.hpp"

#include <iostream>
#include <streambuf>
#include <string>

namespace bdg::bison::pty {

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

struct crlf_output_guard::impl {
  crlf_streambuf cout_filter;
  crlf_streambuf cerr_filter;
  std::streambuf* saved_cout;
  std::streambuf* saved_cerr;

  impl(std::function<void(std::string_view)> cout_sink, std::function<void(std::string_view)> cerr_sink)
      : cout_filter(std::move(cout_sink)),
        cerr_filter(std::move(cerr_sink)),
        saved_cout(std::cout.rdbuf(&cout_filter)),
        saved_cerr(std::cerr.rdbuf(&cerr_filter)) {}
};

crlf_output_guard::crlf_output_guard() {
  auto* orig_cout = std::cout.rdbuf();
  auto* orig_cerr = std::cerr.rdbuf();
  impl_ = std::make_unique<impl>(
      [orig_cout](std::string_view s) { orig_cout->sputn(s.data(), static_cast<std::streamsize>(s.size())); },
      [orig_cerr](std::string_view s) { orig_cerr->sputn(s.data(), static_cast<std::streamsize>(s.size())); });
}

crlf_output_guard::crlf_output_guard(std::function<void(std::string_view)> sink) {
  impl_ = std::make_unique<impl>(sink, sink);
}

crlf_output_guard::~crlf_output_guard() {
  std::cout.rdbuf(impl_->saved_cout);
  std::cerr.rdbuf(impl_->saved_cerr);
}

} // namespace bdg::bison::pty

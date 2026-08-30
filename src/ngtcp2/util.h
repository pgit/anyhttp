/*
 * ngtcp2
 *
 * Copyright (c) 2017 ngtcp2 contributors
 * Copyright (c) 2012 nghttp2 contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#ifndef UTIL_H
#define UTIL_H

#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <string>

#include <ngtcp2/ngtcp2.h>

namespace ngtcp2 {

namespace util {

inline constexpr auto hexdigits = []() {
  constexpr char LOWER_XDIGITS[] = "0123456789abcdef";

  std::array<char, 512> tbl;

  for (size_t i = 0; i < 256; ++i) {
    tbl[i * 2] = LOWER_XDIGITS[static_cast<size_t>(i >> 4)];
    tbl[i * 2 + 1] = LOWER_XDIGITS[static_cast<size_t>(i & 0xf)];
  }

  return tbl;
}();

// format_hex converts a range [|first|, |last|) in hex format, and
// stores the result in another range, beginning at |result|.  It
// returns an output iterator to the element past the last element
// stored.
template <std::input_iterator I, std::weakly_incrementable O>
requires(std::indirectly_writable<O, char> &&
         sizeof(std::iter_value_t<I>) == sizeof(uint8_t))
constexpr O format_hex(I first, I last, O result) {
  for (; first != last; ++first) {
    result = std::ranges::copy_n(
               hexdigits.data() + static_cast<uint8_t>(*first) * 2, 2, result)
               .out;
  }

  return result;
}

// format_hex converts a range [|first|, |first| + |n|) in hex format,
// and stores the result in another range, beginning at |result|.  It
// returns an output iterator to the element past the last element
// stored.
template <std::input_iterator I, std::weakly_incrementable O>
requires(std::indirectly_writable<O, char> &&
         sizeof(std::iter_value_t<I>) == sizeof(uint8_t))
constexpr O format_hex(I first, std::iter_difference_t<I> n, O result) {
  return format_hex(first, std::ranges::next(first, n), std::move(result));
}

// format_hex converts a range [|first|, |first| + |n|) in hex format,
// and returns it.
template <std::input_iterator I>
requires(sizeof(std::iter_value_t<I>) == sizeof(uint8_t))
constexpr std::string format_hex(I first, std::iter_difference_t<I> n) {
  if (n <= 0) {
    return {};
  }

  std::string res;

  res.resize(static_cast<size_t>(n * 2));

  format_hex(std::move(first), std::move(n), std::ranges::begin(res));

  return res;
}

// timestamp returns the current timestamp of steady clock, in nanoseconds.
ngtcp2_tstamp timestamp();

// straddr stringifies |sa| of length |salen| in a format "[IP]:PORT".
std::string straddr(const sockaddr *sa, socklen_t salen);

} // namespace util

} // namespace ngtcp2

#endif // !defined(UTIL_H)

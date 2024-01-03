#pragma once

#include <boost/asio/ip/tcp.hpp>

#include <fmt/format.h>
#include <fmt/ostream.h>

#include <fmtlog.h>

#include <nghttp2/nghttp2.h>

// https://fmt.dev/latest/api.html#std-ostream-support
template <>
struct fmt::formatter<boost::asio::ip::tcp::endpoint> : ostream_formatter
{
};

// Create nghttp2_nv from string literal |name| and std::string |value|.
template <size_t N>
nghttp2_nv make_nv_ls(const char (&name)[N], const std::string& value)
{
   return {(uint8_t*)name, (uint8_t*)value.c_str(), N - 1, value.size(),
           NGHTTP2_NV_FLAG_NO_COPY_NAME};
}

template <class T>
constexpr auto make_string_view(const T* data, size_t len)
{
   return std::string_view(static_cast<const char*>(static_cast<const void*>(data)), len);
}

// inspired by <http://blog.korfuri.fr/post/go-defer-in-cpp/>, but our
// template can take functions returning other than void.
template <typename F, typename... T>
struct Defer
{
   explicit Defer(F&& f, T&&... t) : f(std::bind(std::forward<F>(f), std::forward<T>(t)...)) {}
   Defer(Defer&& o) noexcept : f(std::move(o.f)) {}
   ~Defer() { f(); }

   using ResultType = std::invoke_result_t<F, T...>;
   std::function<ResultType()> f;
};

template <typename F, typename... T>
Defer<F, T...> defer(F&& f, T&&... t)
{
   return Defer<F, T...>(std::forward<F>(f), std::forward<T>(t)...);
}


#pragma once

#include <boost/asio/ip/tcp.hpp>

#include <fmt/format.h>
#include <fmt/ostream.h>

#include <spdlog/spdlog.h>

#include <nghttp2/nghttp2.h>

namespace anyhttp
{
using namespace std::chrono_literals;
using namespace boost::asio;

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

inline ip::address normalize(ip::address addr)
{
   if (addr.is_v6())
   {
      auto v6 = addr.to_v6();
      if (v6.is_v4_mapped())
         return v6.to_v4();
   }
   return addr;
}

inline ip::tcp::endpoint normalize(const ip::tcp::endpoint& endpoint)
{
   return {normalize(endpoint.address()), endpoint.port()};
}
}; // namespace anyhttp

// https://fmt.dev/latest/api.html#std-ostream-support
template <>
struct fmt::formatter<boost::asio::ip::tcp::endpoint> : ostream_formatter
{
};

// https://fmt.dev/latest/api.html#std-ostream-support
template <>
struct fmt::formatter<std::__thread_id> : ostream_formatter
{
};


// -------------------------------------------------------------------------------------------------

#if 0
#define loge(...) SPDLOG_ERROR(__VA_ARGS__)
#define logw(...) SPDLOG_WARN(__VA_ARGS__)
#define logi(...) SPDLOG_INFO(__VA_ARGS__)
#define logd(...) SPDLOG_DEBUG(__VA_ARGS__)
#else
#define loge(...)                                                                                  \
   do                                                                                              \
   {                                                                                               \
      if (spdlog::default_logger_raw()->should_log(spdlog::level::err))                            \
         spdlog::default_logger_raw()->error(__VA_ARGS__);                                         \
   } while (false)

#define logw(...)                                                                                  \
   do                                                                                              \
   {                                                                                               \
      if (spdlog::default_logger_raw()->should_log(spdlog::level::warn))                           \
         spdlog::default_logger_raw()->warn(__VA_ARGS__);                                          \
   } while (false)

#define logi(...)                                                                                  \
   do                                                                                              \
   {                                                                                               \
      if (spdlog::default_logger_raw()->should_log(spdlog::level::info))                           \
         spdlog::default_logger_raw()->info(__VA_ARGS__);                                          \
   } while (false)

#define logd(...)                                                                                  \
   do                                                                                              \
   {                                                                                               \
      if (spdlog::default_logger_raw()->should_log(spdlog::level::debug))                          \
         spdlog::default_logger_raw()->debug(__VA_ARGS__);                                         \
   } while (false)
#endif

// =================================================================================================

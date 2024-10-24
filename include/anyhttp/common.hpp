#pragma once

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <boost/core/detail/string_view.hpp>
#include <boost/system/system_error.hpp>
#include <fmt/format.h>
#include <fmt/ostream.h>

#include <spdlog/spdlog.h>

#include <nghttp2/nghttp2.h>

#include <map>

namespace anyhttp
{
namespace asio = boost::asio;

// =================================================================================================

using Fields = std::map<std::string, std::string>;

using ReadSome = void(boost::system::error_code, std::vector<std::uint8_t>);
using ReadSomeHandler = asio::any_completion_handler<ReadSome>;

using Write = void(boost::system::error_code);
using WriteHandler = asio::any_completion_handler<Write>;

// =================================================================================================

// Create nghttp2_nv from string literal |name| and std::string |value|.
// FIXME: don't use this, it is dangerous (prone to dangling string references)
template <size_t N>
nghttp2_nv make_nv_ls(const char (&name)[N], const std::string& value)
{
   return {(uint8_t*)name, (uint8_t*)value.c_str(), N - 1, value.size(),
           NGHTTP2_NV_FLAG_NO_COPY_NAME};
}

inline nghttp2_nv make_nv_ls(const std::string& key, const std::string& value)
{
   return {(uint8_t*)key.c_str(), (uint8_t*)value.c_str(), key.size(), value.size(), 0};
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

inline asio::ip::address normalize(asio::ip::address addr)
{
   if (addr.is_v6())
   {
      auto v6 = addr.to_v6();
      if (v6.is_v4_mapped())
         return v6.to_v4();
   }
   return addr;
}

inline asio::ip::tcp::endpoint normalize(const asio::ip::tcp::endpoint& endpoint)
{
   return {normalize(endpoint.address()), endpoint.port()};
}

}; // namespace anyhttp

// =================================================================================================

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

// https://fmt.dev/latest/api.html#std-ostream-support
template <>
struct fmt::formatter<boost::core::string_view> : ostream_formatter
{
};

// https://github.com/fmtlib/fmt/issues/2865
template <>
struct fmt::formatter<std::filesystem::path> : formatter<std::string_view>
{
   template <typename FormatContext>
   auto format(const std::filesystem::path& path, FormatContext& ctx)
   {
      return formatter<std::string_view>::format(path.string(), ctx);
   }
};

// -------------------------------------------------------------------------------------------------

inline std::string what(const std::exception_ptr& ptr)
{
   std::string result;
   try
   {
      std::rethrow_exception(ptr);
   }
   catch (boost::system::system_error& ex)
   {
      result = fmt::format("exception: {}", ex.code().message());
   }
   catch (std::exception& ex)
   {
      result = fmt::format("exception: {}", ex.what());
   }
   return result;
}

// -------------------------------------------------------------------------------------------------

#if 0
#define loge(...) SPDLOG_ERROR(__VA_ARGS__)
#define logw(...) SPDLOG_WARN(__VA_ARGS__)
#define logi(...) SPDLOG_INFO(__VA_ARGS__)
#define logd(...) SPDLOG_DEBUG(__VA_ARGS__)
#else
#define logwi(ec, ...)                                                                             \
   do                                                                                              \
   {                                                                                               \
      if (ec && spdlog::default_logger_raw()->should_log(spdlog::level::warn))                     \
         spdlog::default_logger_raw()->warn(__VA_ARGS__);                                         \
      else if (spdlog::default_logger_raw()->should_log(spdlog::level::info))                      \
         spdlog::default_logger_raw()->info(__VA_ARGS__);                                          \
   } while (false)

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

#define mloge(x, ...) loge("[{}] " x, logPrefix() __VA_OPT__(, ) __VA_ARGS__)
#define mlogd(x, ...) logd("[{}] " x, logPrefix() __VA_OPT__(, ) __VA_ARGS__)
#define mlogi(x, ...) logi("[{}] " x, logPrefix() __VA_OPT__(, ) __VA_ARGS__)
#define mlogw(x, ...) logw("[{}] " x, logPrefix() __VA_OPT__(, ) __VA_ARGS__)

// =================================================================================================

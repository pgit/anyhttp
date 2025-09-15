#pragma once

#include <anyhttp/common.hpp>
#include <anyhttp/concepts.hpp>

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <boost/beast/http/fields.hpp>

#include <boost/beast/http/fields_fwd.hpp>
#include <boost/beast/http/type_traits.hpp>
#include <boost/core/detail/string_view.hpp>
#include <boost/system/system_error.hpp>
#include <boost/url/authority_view.hpp>
#include <boost/url/pct_string_view.hpp>

#include <spdlog/spdlog.h>

#include <chrono>
#include <iostream>

// =================================================================================================

#define LOG(FMT, ...) std::println(FMT __VA_OPT__(, ) __VA_ARGS__)
// #define LOG(FMT, ...) std::println(std::cout, FMT __VA_OPT__(, ) __VA_ARGS__)

// =================================================================================================

namespace anyhttp
{
namespace asio = boost::asio;
using asio::awaitable;

// =================================================================================================

enum class Protocol
{
   h1,
   http11 = h1,
   h2,
   h3
};

std::string to_string(Protocol protocol);
std::ostream& operator<<(std::ostream& str, Protocol protocol);

// =================================================================================================

// using Fields = std::map<std::string, std::string>;
using Fields = boost::beast::http::fields;
static_assert(boost::beast::http::is_fields<Fields>::value);

using ReadSome = void(boost::system::error_code, size_t);
using ReadSomeHandler = asio::any_completion_handler<ReadSome>;

using Write = void(boost::system::error_code);
using WriteHandler = asio::any_completion_handler<Write>;

using DefaultCompletionToken = asio::default_completion_token_t<asio::any_io_executor>;

// =================================================================================================

/**
 * Custom invoke template function that moves the invoked function away before actually calling it.
 * This is important in places where the user-provided handler may re-install itself again.
 *
 * This also ensures that the callback is destroyed after invocation.
 */
template <typename F, typename... Args>
   requires std::invocable<F, Args...>
inline void swap_and_invoke(F&& function, Args&&... args)
{
   std::decay_t<F> temp;
   std::swap(function, temp);
   std::move(temp)(std::forward<Args>(args)...);
}

// =================================================================================================

namespace impl
{
class Reader
{
public:
   virtual ~Reader() = default;
   virtual asio::any_io_executor get_executor() const noexcept = 0;
   virtual std::optional<size_t> content_length() const noexcept = 0;
   virtual void async_read_some(asio::mutable_buffer buffer, ReadSomeHandler&& handler) = 0;
   virtual void detach() = 0;

   virtual void destroy(std::unique_ptr<Reader> self) noexcept { /* delete self */ }
};

class Writer
{
public:
   virtual ~Writer() = default;
   virtual asio::any_io_executor get_executor() const noexcept = 0;
   virtual void content_length(std::optional<size_t> content_length) = 0;
   virtual void async_write(WriteHandler&& handler, asio::const_buffer buffer) = 0;
   virtual void detach() = 0;

   virtual void destroy(std::unique_ptr<Writer> self) noexcept { /* delete self */ }
};
} // namespace impl

// =================================================================================================

template <class T>
constexpr std::string_view make_string_view(const T* data, size_t len)
{
   return {static_cast<const char*>(static_cast<const void*>(data)), len};
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

asio::ip::address normalize(asio::ip::address addr);
asio::ip::tcp::endpoint normalize(const asio::ip::tcp::endpoint& endpoint);

}; // namespace anyhttp

// -------------------------------------------------------------------------------------------------

boost::system::error_code code(const std::exception_ptr& ptr);

/// Get error message from exception pointer, as used in the completion signature of \c co_spawn().
std::string what(const std::exception_ptr& ptr);

/// Get error message from \c boost::system::error_code, used by ASIO.
std::string what(const boost::system::error_code& ec);

// -------------------------------------------------------------------------------------------------

/// Format according to HTTP date spec (RFC 7231)
std::string format_http_date(std::chrono::system_clock::time_point tp);

// -------------------------------------------------------------------------------------------------

#define logwi(ec, ...)                                                                             \
   do                                                                                              \
   {                                                                                               \
      if (ec && spdlog::default_logger_raw()->should_log(spdlog::level::warn))                     \
         spdlog::default_logger_raw()->warn(__VA_ARGS__);                                          \
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

#define mloge(x, ...) loge("[{}] " x, logPrefix() __VA_OPT__(, ) __VA_ARGS__)
#define mlogd(x, ...) logd("[{}] " x, logPrefix() __VA_OPT__(, ) __VA_ARGS__)
#define mlogi(x, ...) logi("[{}] " x, logPrefix() __VA_OPT__(, ) __VA_ARGS__)
#define mlogw(x, ...) logw("[{}] " x, logPrefix() __VA_OPT__(, ) __VA_ARGS__)

// =================================================================================================

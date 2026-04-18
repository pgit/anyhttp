#pragma once

#include <anyhttp/common.hpp>
#include <anyhttp/concepts.hpp>
#include <anyhttp/logging.hpp>

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>

#if defined HAVE_CAPY
#include <boost/capy/continuation.hpp>
#include <boost/capy/ex/io_env.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/io_result.hpp>
#include <boost/capy/io_task.hpp>
#endif

#include <boost/beast/http/fields.hpp>

#include <boost/beast/http/fields_fwd.hpp>
#include <boost/beast/http/type_traits.hpp>
#include <boost/system/system_error.hpp>
#include <boost/url/authority_view.hpp>
#include <boost/url/pct_string_view.hpp>

#include <chrono>
#include <iostream>
#include <memory>

// =================================================================================================

namespace anyhttp
{
namespace asio = boost::asio;
#if defined HAVE_CAPY
namespace capy = boost::capy;
#endif

template <typename T>
using Awaitable = asio::awaitable<T>;
using Executor = asio::any_io_executor;

using TcpAcceptor = asio::ip::tcp::acceptor;
using UdpSocket = asio::ip::udp::socket;

#if defined HAVE_CAPY
template <typename... T>
using CapyIoTask = capy::io_task<T...>;
using CapyExecutor = capy::executor_ref;
#endif

// -------------------------------------------------------------------------------------------------

using error_code = boost::system::error_code;

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

using Fields = boost::beast::http::fields;
static_assert(boost::beast::http::is_fields<Fields>::value);

using ReadSome = void(boost::system::error_code, size_t);
using ReadSomeHandler = asio::any_completion_handler<ReadSome>;

using WriteSome = void(boost::system::error_code, size_t);
using writeSomeHandler = asio::any_completion_handler<WriteSome>;

using Write = void(boost::system::error_code);
using WriteHandler = asio::any_completion_handler<Write>;

using DefaultCompletionToken = asio::default_completion_token_t<Executor>;

#if defined HAVE_CAPY
template <typename Result>
struct CapyAwaitableState
{
   capy::io_env const* env = nullptr;
   capy::continuation continuation;
   error_code ec;
   Result result{};

   [[nodiscard]] capy::io_result<Result> resume_result() noexcept
   {
      if (env != nullptr && env->stop_token.stop_requested())
         return {make_error_code(std::errc::operation_canceled), Result{}};

      return {ec, std::move(result)};
   }
};

struct CapyAwaitableStateVoid
{
   capy::io_env const* env = nullptr;
   capy::continuation continuation;
   error_code ec;

   [[nodiscard]] capy::io_result<> resume_result() const noexcept
   {
      if (env != nullptr && env->stop_token.stop_requested())
         return {make_error_code(std::errc::operation_canceled)};

      return {ec};
   }
};
#endif

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
   std::exchange(function, nullptr)(std::forward<Args>(args)...);
}

// =================================================================================================

namespace impl
{
class Reader : public std::enable_shared_from_this<Reader>
{
public:
   virtual ~Reader() = default;
   virtual Executor get_executor() const noexcept = 0;
   virtual std::optional<size_t> content_length() const noexcept = 0;
   virtual void async_read_some(asio::mutable_buffer buffer, ReadSomeHandler&& handler) = 0;
   virtual void detach() = 0;
   virtual void destroy() {};
};

class Writer : public std::enable_shared_from_this<Writer>
{
public:
   virtual ~Writer() = default;
   virtual Executor get_executor() const noexcept = 0;
   virtual void content_length(std::optional<size_t> content_length) = 0;
   virtual void async_write(WriteHandler&& handler, asio::const_buffer buffer) = 0;
   virtual void detach() = 0;
   virtual void destroy() {};
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

// =================================================================================================

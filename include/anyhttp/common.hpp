#pragma once

#include <anyhttp/common.hpp>
#include <anyhttp/concepts.hpp>
#include <anyhttp/logging.hpp>

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/associated_immediate_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <boost/beast/http/fields.hpp>

#include <boost/beast/http/fields_fwd.hpp>
#include <boost/beast/http/type_traits.hpp>
#include <boost/system/system_error.hpp>
#include <boost/url/authority_view.hpp>
#include <boost/url/pct_string_view.hpp>

#include <chrono>
#include <format>
#include <iostream>
#include <memory>
#include <string_view>

// =================================================================================================

namespace anyhttp
{
namespace asio = boost::asio;
using asio::awaitable;

using error_code = boost::system::error_code;

// =================================================================================================

enum class Protocol
{
   http11,
   h2,
   h3
};

std::string to_string(Protocol protocol);
std::ostream& operator<<(std::ostream& str, Protocol protocol);

// =================================================================================================

using Fields = boost::beast::http::fields;
static_assert(boost::beast::http::is_fields<Fields>::value);

//
// A header value as passed to fields() below: either something string-like, or anything
// std::format can turn into a string, so sizes and counts need no conversion at the call site.
//
// String-like values are only referenced, never copied: a FieldValue lives just long enough for
// fields() to hand the bytes to Beast, which copies them. Only formatted values need storage.
//
class FieldValue
{
public:
   template <typename T>
      requires std::formattable<const T&, char>
   FieldValue(const T& value)
   {
      if constexpr (std::convertible_to<const T&, std::string_view>)
         view = value;
      else
      {
         buffer = std::format("{}", value);
         view = buffer;
      }
   }

   /// 'view' may point into 'buffer', so a copy would alias the original's storage.
   FieldValue(const FieldValue&) = delete;

   operator std::string_view() const noexcept { return view; }

private:
   std::string_view view;
   std::string buffer; // only ever used for a value that had to be formatted
};

//
// Beast's fields have no initializer-list constructor, so building a small, fixed set of headers
// takes a statement per header. This lets it be spelled inline:
//
//    fields({{"Content-Length", body.size()}, {"Content-Type", "text/plain"}})
//
inline Fields fields(std::initializer_list<std::pair<std::string_view, FieldValue>> headers)
{
   Fields result;
   for (auto&& [name, value] : headers)
      result.set(name, std::string_view(value));
   return result;
}

// =================================================================================================

using ReadSome = void(boost::system::error_code, size_t);
using ReadSomeHandler = asio::any_completion_handler<ReadSome>;

using WriteSome = void(boost::system::error_code, size_t);
using writeSomeHandler = asio::any_completion_handler<WriteSome>;

using Write = void(boost::system::error_code);
using WriteHandler = asio::any_completion_handler<Write>;

using Status = void(boost::system::error_code);
using StatusHandler = asio::any_completion_handler<Status>;

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
   std::exchange(function, nullptr)(std::forward<Args>(args)...);
}

// =================================================================================================

/**
 * Completes \p handler without doing any I/O, through its associated immediate executor (with
 * \p fallback standing in when the handler has none). This is the one way an operation that has
 * nothing asynchronous left to do may finish: invoking the handler straight from the initiating
 * function would surprise callers that rely on the ASIO guarantee of not being re-entered.
 *
 * A handler that is empty (an \c any_completion_handler detached by cancellation) is quietly
 * dropped -- there is nobody left to tell.
 */
template <typename Handler, typename... Args>
inline void complete_immediately(Handler&& handler, const asio::any_io_executor& fallback,
                                 Args&&... args)
{
   if (!handler)
      return;

   asio::any_completion_executor ex = asio::get_associated_immediate_executor(handler, fallback);
   ex.execute([handler = std::forward<Handler>(handler),
               ... args = std::forward<Args>(args)]() mutable { //
      std::move(handler)(std::move(args)...);
   });
}

// =================================================================================================

namespace impl
{
class Reader : public std::enable_shared_from_this<Reader>
{
public:
   virtual ~Reader() = default;
   virtual asio::any_io_executor get_executor() const noexcept = 0;
   virtual std::optional<size_t> content_length() const noexcept = 0;

   //
   // Reads at most one buffer worth of the incoming body. The end of the body is reported the way
   // ASIO reports it everywhere else: \c asio::error::eof with zero bytes, and again for every
   // further read -- including reads issued after the underlying stream object is long gone. A
   // body that ends before it was supposed to -- a reset stream, a connection that went away
   // mid-message -- is reported as \c http::error::partial_message instead, so the two cases stay
   // distinguishable.
   //
   // An empty buffer is not a request to do anything; it completes immediately with success and
   // zero bytes, wherever the body stands.
   //
   virtual void async_read_some(asio::mutable_buffer buffer, ReadSomeHandler&& handler) = 0;
   virtual void detach() = 0;
   virtual void destroy() {};
};

class Writer : public std::enable_shared_from_this<Writer>
{
public:
   virtual ~Writer() = default;
   virtual asio::any_io_executor get_executor() const noexcept = 0;
   virtual void content_length(std::optional<size_t> content_length) = 0;

   //
   // Writes \p buffer and, if \p eof is set, ends the outgoing body after it. The two travel
   // together on purpose: every backend can put the last bytes of a body and the flag that ends
   // it into the same protocol element -- one DATA frame with END_STREAM (HTTP/2), one QUIC
   // STREAM frame with FIN (HTTP/3), one last chunk (HTTP/1.1) -- so a message that ends with
   // data needs no second, empty write to close it out.
   //
   // Every implementation answers the same entry ladder, in this order: an empty buffer with
   // \p eof clear writes nothing at all and completes immediately with success, wherever the
   // body stands -- it is not, as it once was, how a body is ended. Once the body has been ended,
   // writing data -- through either entry point -- completes with \c errc::broken_pipe, while
   // re-ending it with no data attached is an idempotent no-op. Only then do stream-level
   // failures (closed, cancelled) get their say.
   //
   virtual void async_write(WriteHandler&& handler, asio::const_buffer buffer, bool eof) = 0;
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

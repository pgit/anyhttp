#pragma once

#include <anyhttp/logging.hpp>
#include <anyhttp/runtime.hpp>

#include <boost/beast/http/fields.hpp>

#include <boost/beast/http/fields_fwd.hpp>
#include <boost/beast/http/type_traits.hpp>
#include <boost/system/system_error.hpp>
#include <boost/url/authority_view.hpp>
#include <boost/url/pct_string_view.hpp>

#include <array>
#include <chrono>
#include <format>
#include <iostream>
#include <string_view>

// =================================================================================================

namespace anyhttp
{
// =================================================================================================

/// What a connection speaks.
enum class Protocol
{
   h1, ///< HTTP/1.1, over TCP, with or without TLS
   h2, ///< HTTP/2, over TCP, with or without TLS
   h3 ///< HTTP/3, over QUIC
};

/// The name of a protocol, as it appears in the logs.
/// \param protocol The protocol.
/// \return "HTTP11", "HTTP2" or "HTTP3".
std::string to_string(Protocol protocol);

/// Writes what \c to_string() says.
/// \param str The stream to write to.
/// \param protocol The protocol.
/// \return \p str
std::ostream& operator<<(std::ostream& str, Protocol protocol);

// =================================================================================================

/// The header fields of a message, in both directions: Beast's container for them.
using Fields = boost::beast::http::fields;
static_assert(boost::beast::http::is_fields<Fields>::value);

/// Default for \c server::Config::max_header_size and \c client::Config::max_header_size.
inline constexpr size_t default_max_header_size = 64 * 1024;

/**
 * What a received header field counts against the limit on the size of a header section: its name
 * and value plus 32 bytes of overhead, as for SETTINGS_MAX_HEADER_LIST_SIZE in HTTP/2 (RFC 9113,
 * section 6.5.2) and SETTINGS_MAX_FIELD_SECTION_SIZE in HTTP/3 (RFC 9114, section 4.2.2).
 *
 * \param name The name of the field.
 * \param value The value of the field.
 * \return The size it counts as, in bytes.
 */
constexpr size_t header_field_size(std::string_view name, std::string_view value) noexcept
{
   return name.size() + value.size() + 32;
}

/**
 * A header value as passed to \c fields(): either something string-like, or anything
 * \c std::format can turn into a string, so sizes and counts need no conversion at the call site.
 *
 * String-like values are only referenced, never copied: a FieldValue lives just long enough for
 * \c fields() to hand the bytes to Beast, which copies them. Only formatted values need storage.
 */
class FieldValue
{
public:
   /// Takes \p value by reference if it is string-like, and formats it otherwise.
   /// \param value The value of the header field.
   template <typename T>
      requires std::formattable<const T&, char>
   FieldValue(const T& value) // NOLINT(google-explicit-constructor)
   {
      if constexpr (std::convertible_to<const T&, std::string_view>)
         view = value;
      else if constexpr (std::integral<T> && !std::same_as<T, bool> && !std::same_as<T, char>)
      {
         buffer = std::to_string(value); // as std::format() would, without parsing a format
         view = buffer;
      }
      else
      {
         buffer = std::format("{}", value);
         view = buffer;
      }
   }

   /// \cond internal
   /// 'view' may point into 'buffer', so a copy would alias the original's storage.
   FieldValue(const FieldValue&) = delete;
   /// \endcond

   /// \return The value, which lives as long as the FieldValue and what it refers to.
   operator std::string_view() const noexcept // NOLINT(google-explicit-constructor)
   {
      return view;
   }

private:
   std::string_view view;
   std::string buffer; // only ever used for a value that had to be formatted
};

/**
 * Beast's fields have no initializer-list constructor, so building a small, fixed set of headers
 * takes a statement per header. This lets it be spelled inline:
 *
 * \code
 * fields({{"Content-Length", body.size()}, {"Content-Type", "text/plain"}})
 * \endcode
 *
 * \param headers The names and values of the header fields.
 * \return The header fields, which own their copy of all of that.
 */
inline Fields fields(std::initializer_list<std::pair<std::string_view, FieldValue>> headers)
{
   Fields result;
   for (auto&& [name, value] : headers)
      result.set(name, std::string_view(value));
   return result;
}

// =================================================================================================

/// The completion signature of \c Reader::async_read_some: the error and the bytes read.
using ReadSome = void(error_code, size_t);
using ReadSomeHandler = Completion<ReadSome>;

using WriteSome = void(error_code, size_t);
using WriteSomeHandler = Completion<WriteSome>;

/// The completion signature of \c Writer::async_write and \ref Writer::async_write_eof.
using Write = void(error_code);
using WriteHandler = Completion<Write>;

/// The completion signature of an operation that has nothing to report but how it went.
using Status = void(error_code);
using StatusHandler = Completion<Status>;

#if ANYHTTP_ASIO
/// What an operation of a runtime that has completion tokens takes when it is given none.
using DefaultCompletionToken = asio::default_completion_token_t<Executor>;
#endif

// =================================================================================================

/// \cond internal

/**
 * Custom invoke template function that moves the invoked function away before actually calling it.
 * This is important in places where the user-provided handler may re-install itself again.
 *
 * This also ensures that the callback is destroyed after invocation.
 */
template <typename F, typename... Args>
   requires std::invocable<std::remove_cvref_t<F>, Args...>
inline void swap_and_invoke(F&& function, Args&&... args)
{
   std::exchange(function, nullptr)(std::forward<Args>(args)...);
}

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

/// \p address, or the IPv4 address an IPv4-mapped IPv6 one maps. Defined by each runtime.
IpAddress normalize(IpAddress address);
TcpEndpoint normalize(const TcpEndpoint& endpoint);

/// Which end of a connection a session is, for the colour of its log prefix.
enum class Role
{
   server,
   client
};

/// The prefix of log lines that belong to no connection: "server" in red, "client" in green.
std::string log_prefix(Role role);

/**
 * The prefix a session puts in front of its log lines: "<protocol>:<address>:<port>" of the peer.
 * The protocol is red for a server and green for a client, the address coloured as by "ip -c"
 * (IPv4 magenta, IPv6 blue and in square brackets).
 */
std::string log_prefix(Role role, std::string_view protocol, const IpAddress& address,
                       unsigned short port);

/// As above, for the peer at \p remote. Just \p protocol without one (the socket is not connected).
std::string log_prefix(Role role, std::string_view protocol,
                       const std::optional<TcpEndpoint>& remote);

/// \endcond

}; // namespace anyhttp

/// \cond internal

// -------------------------------------------------------------------------------------------------

/// Get error message from exception pointer, as used in the completion signature of \c co_spawn().
std::string what(const std::exception_ptr& ptr);

/// Get error message from a boost::system_error, as thrown by boost ASIO if not caught.
std::string what(const boost::system::system_error& ex);

/// Get error message from an \c anyhttp::error_code, as reported by the runtime.
std::string what(const anyhttp::error_code& ec);

// -------------------------------------------------------------------------------------------------

/**
 * An HTTP date, "Sun, 06 Nov 1994 08:49:37 GMT" (IMF-fixdate, RFC 9110, section 5.6.7). It has a
 * fixed length, so it is kept in a buffer of its own rather than on the heap: every response
 * carries one.
 */
struct HttpDate
{
   std::array<char, 29> chars;
   operator std::string_view() const noexcept // NOLINT(google-explicit-constructor)
   {
      return {chars.data(), chars.size()};
   }
};

/// Formats \p tp as an HTTP date, clamped to the years 0000 to 9999 the format has room for.
HttpDate format_http_date(std::chrono::system_clock::time_point tp);

/// The current time as an HTTP date, for the \c Date header of a response.
HttpDate http_date_now();

/// \endcond

// =================================================================================================

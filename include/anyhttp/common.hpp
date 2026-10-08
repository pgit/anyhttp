#pragma once

#include <anyhttp/common.hpp>
#include <anyhttp/logging.hpp>
#include <anyhttp/runtime.hpp>

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
// =================================================================================================

enum class Protocol
{
   h1,
   h2,
   h3
};

std::string to_string(Protocol protocol);
std::ostream& operator<<(std::ostream& str, Protocol protocol);

// =================================================================================================

using Fields = boost::beast::http::fields;
static_assert(boost::beast::http::is_fields<Fields>::value);

/// Default for \c server::Config::max_header_size and \c client::Config::max_header_size.
inline constexpr size_t default_max_header_size = 64 * 1024;

/**
 * What a received header field counts against the limit on the size of a header section: its name
 * and value plus 32 bytes of overhead, as for SETTINGS_MAX_HEADER_LIST_SIZE in HTTP/2 (RFC 9113,
 * section 6.5.2) and SETTINGS_MAX_FIELD_SECTION_SIZE in HTTP/3 (RFC 9114, section 4.2.2).
 */
constexpr size_t header_field_size(std::string_view name, std::string_view value) noexcept
{
   return name.size() + value.size() + 32;
}

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

using ReadSome = void(error_code, size_t);
using ReadSomeHandler = Completion<ReadSome>;

using WriteSome = void(error_code, size_t);
using WriteSomeHandler = Completion<WriteSome>;

using Write = void(error_code);
using WriteHandler = Completion<Write>;

using Status = void(error_code);
using StatusHandler = Completion<Status>;

#if ANYHTTP_ASIO
using DefaultCompletionToken = asio::default_completion_token_t<Executor>;
#endif

// =================================================================================================

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

}; // namespace anyhttp

// -------------------------------------------------------------------------------------------------

/// Get error message from exception pointer, as used in the completion signature of \c co_spawn().
std::string what(const std::exception_ptr& ptr);

/// Get error message from a boost::system_error, as thrown by boost ASIO if not caught.
std::string what(const boost::system::system_error& ex);

/// Get error message from an \c anyhttp::error_code, as reported by the runtime.
std::string what(const anyhttp::error_code& ec);

// -------------------------------------------------------------------------------------------------

/// Format according to HTTP date spec (RFC 7231)
std::string format_http_date(std::chrono::system_clock::time_point tp);

// =================================================================================================

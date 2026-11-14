#pragma once

//
// HTTP/1.1 over a stream, with Boost.HTTP's parser and serializer. Both are sans-I/O: the parser
// takes input through prepare()/commit() and hands out the body with pull_body()/consume_body(),
// the serializer takes the body through stream_prepare()/stream_commit() and hands out what goes
// on the wire with prepare()/consume(). Boost.HTTP's own loops around them (parser::read(),
// serializer::sink) are written for capy's streams, so these are the same loops on the runtime
// layer, to run on either runtime. The framing -- chunks, Content-Length, the end of the body --
// stays the library's.
//
// What comes out of here is the runtime's error_code. Boost.HTTP's own codes stay what they are,
// with three exceptions, see to_error_code(): a message cut short is errors::partial_message, as
// the API has it, and every one of the parser's limits on the header section is
// errors::header_limit.
//

#include "anyhttp/common.hpp"
#include "anyhttp/net.hpp"
#include "anyhttp/session_impl.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/container/small_vector.hpp>
#include <boost/http/error.hpp>
#include <boost/http/parser.hpp>
#include <boost/http/serializer.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <tuple>

namespace anyhttp::h1
{

namespace http = boost::http;

/**
 * A Boost.HTTP error as the runtime's error code. A message that ended before it was complete is
 * \c errors::partial_message, as the API has it. All of the parser's limits on the header section
 * -- its total size, the start line, a single field, the number of fields -- are what
 * Config::max_header_size is about, so they are all \c errors::header_limit.
 */
inline error_code to_error_code(const std::error_code& ec)
{
   if (ec == http::error::incomplete)
      return errors::partial_message;
   if (ec == http::error::headers_limit || ec == http::error::start_line_limit ||
       ec == http::error::field_size_limit || ec == http::error::fields_limit)
      return errors::header_limit;
   return error_code(ec);
}

/**
 * The parser's configuration for a header section of at most \p max_header_size bytes, see
 * Config::max_header_size.
 *
 * Boost.HTTP limits the start line, every field and the number of fields on their own, HTTP/2 and
 * HTTP/3 only the whole: the first two can be as large as the whole here, and there can be as many
 * fields as fit when each one counts its name and value plus 32 bytes, as with HTTP/2 and HTTP/3
 * (see header_field_size()). The parser keeps a table entry for each field it may have,
 * so the count is what bounds that table.
 *
 * There is no limit on the body: it is read piece by piece, never as a whole.
 */
inline http::shared_parser_config make_parser_config(bool server, size_t max_header_size)
{
   http::parser_config config(server);
   config.headers.max_size = max_header_size;
   config.headers.max_start_line = max_header_size;
   config.headers.max_field = max_header_size;
   config.headers.max_fields = std::max<size_t>(max_header_size / header_field_size("", ""), 1);
   config.body_limit = std::numeric_limits<std::uint64_t>::max();
   return http::make_parser_config(config);
}

/**
 * The serializer's configuration. Its buffer is what a write copies the body into, a piece at a
 * time, framing included: the larger, the fewer writes a large body takes.
 */
inline http::shared_serializer_config make_serializer_config()
{
   http::serializer_config config;
   config.payload_buffer = 64 * 1024;
   return http::make_serializer_config(config);
}

// =================================================================================================

/**
 * Gives the parser more input: what is left of \p pending first, the bytes a session started with
 * (see Session::Impl::do_session()), and only then what can be read from \p stream.
 *
 * The end of the stream goes to the parser, which knows whether a message can end with it: the
 * next parse() says. Over TLS, a connection that ends without a "close_notify" may have been cut
 * by anyone, so no message can be complete because of it: once the header is there, that is a
 * \c errors::partial_message for the body. Before, the parser can tell nothing of a message from
 * a message cut short, as with any other end of the stream.
 */
template <typename Stream>
IoTask<> fill(Stream& stream, Buffer& pending, http::parser& parser)
{
   auto buffers = parser.prepare();
   auto it = std::ranges::find_if(buffers, [](const auto& buffer) { return buffer.size() > 0; });
   if (it == buffers.end())
      co_return std::tuple{to_error_code(http::error::buffer_overflow)};
   const boost::asio::mutable_buffer buffer(it->data(), it->size());

   if (pending.size())
   {
      const auto n = boost::asio::buffer_copy(buffer, pending.data());
      pending.consume(n);
      parser.commit(n);
      co_return std::tuple{error_code{}};
   }

   auto [ec, n] = co_await io::read_some(stream, buffer);
   if (ec == errors::eof)
      parser.commit_eof();
   else if (io::is_truncated(ec))
   {
      if (parser.got_header())
         co_return std::tuple{errors::partial_message};
      parser.commit_eof();
   }
   else if (ec)
      co_return std::tuple{ec};
   else
      parser.commit(n);
   co_return std::tuple{error_code{}};
}

/**
 * Reads until the parser has the whole header section.
 *
 * The end of the stream is \c errors::end_of_stream if nothing of a message had arrived yet, and a
 * \c errors::partial_message otherwise.
 */
template <typename Stream>
IoTask<> read_header(Stream& stream, Buffer& pending, http::parser& parser)
{
   for (;;)
   {
      std::error_code ec;
      parser.parse(ec);
      if (parser.got_header())
         co_return std::tuple{error_code{}};
      if (ec != http::condition::need_more_input)
         co_return std::tuple{to_error_code(ec)};

      if (auto [fill_ec] = co_await h1::fill(stream, pending, parser); fill_ec)
         co_return std::tuple{fill_ec};
   }
}

/**
 * Reads some of the body into \p buffer, as much as is there, reading from the stream only if
 * nothing is. Yields the number of bytes copied, and \c errors::eof once the parser has the whole
 * message and all of the body has been taken.
 */
template <typename Stream>
IoTask<size_t> read_body(Stream& stream, Buffer& pending, http::parser& parser,
                         boost::asio::mutable_buffer buffer)
{
   for (;;)
   {
      std::error_code ec;
      parser.parse(ec);

      size_t copied = 0;
      for (const auto& body : parser.pull_body())
      {
         const auto n =
            boost::asio::buffer_copy(boost::asio::buffer(buffer + copied),
                                     boost::asio::const_buffer(body.data(), body.size()));
         copied += n;
         if (n < body.size())
            break;
      }
      parser.consume_body(copied);
      if (copied)
         co_return std::tuple{error_code{}, copied};

      if (parser.is_complete())
         co_return std::tuple{errors::eof, size_t{0}};

      // more input, or room for it, which taking what was there has just made
      if (ec && ec != http::condition::need_more_input && ec != http::error::in_place_overflow)
         co_return std::tuple{to_error_code(ec), size_t{0}};

      if (auto [fill_ec] = co_await h1::fill(stream, pending, parser); fill_ec)
         co_return std::tuple{fill_ec, size_t{0}};
   }
}

// -------------------------------------------------------------------------------------------------

/**
 * Writes what \p serializer has to offer, until it needs more of the body or is done. Yields the
 * number of bytes written, framing and all.
 *
 * The serializer stops after the header of a message that expects "100 Continue" as well, which
 * is no reason to stop here: whoever asked for that has decided to send the body right away.
 */
template <typename Stream>
IoTask<size_t> flush(Stream& stream, http::serializer& serializer)
{
   size_t written = 0;
   while (!serializer.is_done())
   {
      auto prepared = serializer.prepare();
      if (prepared.has_error())
      {
         if (prepared.error() == http::error::need_data)
            break;
         if (prepared.error() == http::error::expect_100_continue)
            continue;
         co_return std::tuple{to_error_code(prepared.error()), written};
      }

      //
      // The buffers prepare() hands out are valid until consume(), the sequence it hands them out
      // in is not: copy the descriptors.
      //
      boost::container::small_vector<boost::asio::const_buffer, 4> buffers;
      for (const auto& buffer : *prepared)
         if (buffer.size())
            buffers.emplace_back(buffer.data(), buffer.size());
      if (buffers.empty())
      {
         serializer.consume(0);
         continue;
      }

      auto [ec, n] = co_await io::write(stream, buffers);
      serializer.consume(n);
      written += n;
      if (ec)
         co_return std::tuple{ec, written};
   }
   co_return std::tuple{error_code{}, written};
}

/**
 * Starts \p message on \p serializer and writes its header, the body to follow by write_body().
 * A message that has no body -- \p body is false -- is done with this.
 */
template <typename Stream>
IoTask<> write_header(Stream& stream, http::serializer& serializer,
                      const http::message_base& message, bool body = true)
{
   serializer.reset();
   serializer.set_message(message);
   if (body)
      serializer.start_buffers();
   else
      serializer.start();
   auto [ec, n] = co_await h1::flush(stream, serializer);
   co_return std::tuple{ec};
}

/**
 * Writes \p buffer as body, and ends the body if \p eof says so. The serializer takes the body by
 * copying it, framing it as it goes, as much at a time as its buffer holds: when \p eof is set,
 * the end of the body goes with the last of it.
 *
 * Yields the number of bytes of \p buffer the serializer has taken. Those are on their way, but not
 * necessarily on the wire when the write fails: with a stream that can not be written to any more,
 * that is of no consequence.
 */
template <typename Stream>
IoTask<size_t> write_body(Stream& stream, http::serializer& serializer,
                          boost::asio::const_buffer buffer, bool eof)
{
   size_t taken = 0;
   for (;;)
   {
      //
      // One commit per prepare: it invalidates what prepare handed out, and with chunked encoding,
      // it is what makes a chunk of what has been copied.
      //
      const auto space = serializer.stream_prepare();
      const std::array<boost::asio::mutable_buffer, 2> spaces{
         boost::asio::mutable_buffer(space[0].data(), space[0].size()),
         boost::asio::mutable_buffer(space[1].data(), space[1].size())};
      const auto n = boost::asio::buffer_copy(spaces, buffer + taken);
      serializer.stream_commit(n);
      taken += n;

      const bool last = taken == buffer.size();
      if (last && eof)
         serializer.stream_close();

      auto [ec, written] = co_await h1::flush(stream, serializer);
      if (ec || last)
         co_return std::tuple{ec, taken};
   }
}

// -------------------------------------------------------------------------------------------------

/**
 * Writes all of \p message, with \p body as its body: the counterpart of read_message(), for
 * talking HTTP/1.1 by hand. The message says how the body is framed, by 'Content-Length' or
 * 'Transfer-Encoding: chunked'.
 */
template <typename Stream>
IoTask<> write_message(Stream& stream, http::serializer& serializer,
                       const http::message_base& message, std::string_view body = {})
{
   if (auto [ec] = co_await h1::write_header(stream, serializer, message); ec)
      co_return std::tuple{ec};
   auto [ec, n] = co_await h1::write_body(stream, serializer, boost::asio::buffer(body), true);
   co_return std::tuple{ec};
}

/**
 * Reads a whole message with \p parser, which must have been started on it, and yields its body:
 * the header is the parser's. For talking HTTP/1.1 by hand.
 */
template <typename Stream>
IoTask<std::string> read_message(Stream& stream, Buffer& pending, http::parser& parser)
{
   std::string body;
   if (auto [ec] = co_await h1::read_header(stream, pending, parser); ec)
      co_return std::tuple{ec, std::move(body)};

   std::array<char, 16384> buffer;
   for (;;)
   {
      auto [ec, n] = co_await h1::read_body(stream, pending, parser, boost::asio::buffer(buffer));
      body.append(buffer.data(), n);
      if (ec == errors::eof)
         co_return std::tuple{error_code{}, std::move(body)};
      if (ec)
         co_return std::tuple{ec, std::move(body)};
   }
}

// =================================================================================================

} // namespace anyhttp::h1

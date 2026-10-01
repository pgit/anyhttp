#pragma once

//
// HTTP/1.1 over a stream, with Beast's parser and serializer, but without Beast's asynchronous
// operations, which need an ASIO stream. These are the same algorithms (see read.hpp and write.hpp
// in boost/beast/http/impl), written as coroutines on the runtime layer so that they run on either
// runtime.
//
// Beast's parser and serializer say what they need through error codes of their own. need_more
// (more input) is handled in here. need_buffer (the body buffer is full, or used up) is what these
// are called to get to, so it comes out as success. Any other error comes out as the runtime's
// error_code, Beast's own ones (end_of_stream, partial_message, header_limit, ...) included.
//

#include "anyhttp/common.hpp"

#include <boost/beast/core/buffers_range.hpp>
#include <boost/beast/core/read_size.hpp>
#include <boost/beast/http/basic_parser.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/serializer.hpp>

#include <boost/container/small_vector.hpp>

#include <cstddef>
#include <tuple>

namespace anyhttp::beast_impl::h1
{

namespace http = boost::beast::http;

/// A Beast error as the runtime's error code.
inline error_code to_error_code(const boost::system::error_code& ec) { return ec; }

// =================================================================================================

/**
 * Parses what \p buffer holds and, if the parser needs more, reads from \p stream into \p buffer
 * first, until the parser has made progress. As Beast's \c http::async_read_some(), this yields the
 * number of bytes the parser has used up.
 *
 * The end of the stream is \c http::error::end_of_stream if nothing of a message had arrived yet.
 * Otherwise the parser decides: a message that ends with the connection is complete, any other one
 * is a \c http::error::partial_message.
 */
template <typename Stream, typename DynamicBuffer, bool isRequest>
Task<std::tuple<error_code, size_t>> read_some(Stream& stream, DynamicBuffer& buffer,
                                               http::basic_parser<isRequest>& parser)
{
   size_t parsed = 0;
   boost::system::error_code ec;
   for (bool read = buffer.size() == 0;; read = true)
   {
      if (read)
      {

         const auto size = boost::beast::read_size(buffer, 65536);
         if (size == 0)
            co_return std::tuple{to_error_code(http::error::buffer_overflow), parsed};

         auto [read_ec, n] = co_await io::read_some(stream, buffer.prepare(size));
         buffer.commit(n);
         if (read_ec == errors::eof)
         {
            if (!parser.got_some())
               co_return std::tuple{to_error_code(http::error::end_of_stream), parsed};

            // the caller sees the end of the stream with the next read
            ec = {};
            parser.put_eof(ec);
            co_return std::tuple{to_error_code(ec), parsed};
         }
         if (read_ec)
            co_return std::tuple{read_ec, parsed};
      }

      const auto used = parser.put(buffer.data(), ec);
      parsed += used;
      buffer.consume(used);
      if (ec != http::error::need_more)
         break;
   }

   if (ec == http::error::need_buffer)
      ec = {};
   co_return std::tuple{to_error_code(ec), parsed};
}

/// Reads until the parser has the whole header, as Beast's \c http::async_read_header().
template <typename Stream, typename DynamicBuffer, bool isRequest>
Task<std::tuple<error_code, size_t>> read_header(Stream& stream, DynamicBuffer& buffer,
                                                 http::basic_parser<isRequest>& parser)
{
   size_t parsed = 0;
   while (!parser.is_header_done())
   {

      auto [ec, n] = co_await h1::read_some(stream, buffer, parser);
      if (ec)
         co_return std::tuple{ec, parsed};
      parsed += n;
   }
   co_return std::tuple{error_code{}, parsed};
}

/// Reads until the parser has the whole message, as Beast's \c http::async_read() with a parser.
template <typename Stream, typename DynamicBuffer, bool isRequest>
Task<std::tuple<error_code, size_t>> read(Stream& stream, DynamicBuffer& buffer,
                                          http::basic_parser<isRequest>& parser)
{
   size_t parsed = 0;
   while (!parser.is_done())
   {
      auto [ec, n] = co_await h1::read_some(stream, buffer, parser);
      if (ec)
         co_return std::tuple{ec, parsed};
      parsed += n;
   }
   co_return std::tuple{error_code{}, parsed};
}

/// Reads a whole message into \p message, as Beast's \c http::async_read() with a message.
template <typename Stream, typename DynamicBuffer, bool isRequest, typename Body,
          typename Allocator>
Task<std::tuple<error_code, size_t>>
read_message(Stream& stream, DynamicBuffer& buffer,
             http::message<isRequest, Body, http::basic_fields<Allocator>>& message)
{
   http::parser<isRequest, Body, Allocator> parser;
   parser.eager(true);
   auto [ec, n] = co_await h1::read(stream, buffer, parser);
   if (!ec)
      message = parser.release();
   co_return std::tuple{ec, n};
}

// -------------------------------------------------------------------------------------------------

/**
 * Writes what \p serializer has to offer next, as Beast's \c http::async_write_some(). Yields the
 * number of bytes written, which is not the size of the body that went with them.
 *
 * With a \c buffer_body, a serializer that has used up the body buffer it was given offers nothing
 * but \c http::error::need_buffer, which is passed on here.
 */
template <typename Stream, bool isRequest, typename Body, typename Fields>
Task<std::tuple<error_code, size_t>>
write_some(Stream& stream, http::serializer<isRequest, Body, Fields>& serializer)
{
   if (serializer.is_done())
      co_return std::tuple{error_code{}, size_t{0}};

   //
   // The buffers next() offers are valid until consume(), but the sequence it offers them in is
   // not: take its descriptors out of the visitor.
   //
   boost::system::error_code ec;
   boost::container::small_vector<asio::const_buffer, 16> buffers;
   serializer.next(ec, [&](boost::system::error_code& next_ec, const auto& sequence) {
      next_ec = {};
      for (asio::const_buffer buffer : boost::beast::buffers_range_ref(sequence))
         buffers.push_back(buffer);
   });
   if (ec)
      co_return std::tuple{to_error_code(ec), size_t{0}};

   auto [write_ec, n] = co_await io::write(stream, buffers);
   if (!write_ec)
      serializer.consume(n);
   co_return std::tuple{write_ec, n};
}

/**
 * Writes what \p serializer has to offer until \p done says so. As \c need_buffer is not an error
 * here, this also stops when a \c buffer_body has been written that is not the last.
 */
template <typename Stream, typename Serializer, typename Predicate>
Task<std::tuple<error_code, size_t>> write_until(Stream& stream, Serializer& serializer,
                                                 Predicate done)
{
   size_t written = 0;
   while (!done(serializer))
   {

      auto [ec, n] = co_await h1::write_some(stream, serializer);
      written += n;
      if (ec == to_error_code(http::error::need_buffer))
         break;
      if (ec)
         co_return std::tuple{ec, written};
   }
   co_return std::tuple{error_code{}, written};
}

/// Writes the header, as Beast's \c http::async_write_header().
template <typename Stream, typename Serializer>
Task<std::tuple<error_code, size_t>> write_header(Stream& stream, Serializer& serializer)
{
   serializer.split(true);
   return h1::write_until(stream, serializer, [](auto& sr) { return sr.is_header_done(); });
}

/// Writes all there is, as Beast's \c http::async_write() with a serializer.
template <typename Stream, typename Serializer>
Task<std::tuple<error_code, size_t>> write(Stream& stream, Serializer& serializer)
{
   return h1::write_until(stream, serializer, [](auto& sr) { return sr.is_done(); });
}

/// Writes all of \p message, as Beast's \c http::async_write() with a message.
template <typename Stream, bool isRequest, typename Body, typename Fields>
Task<std::tuple<error_code, size_t>> write_message(Stream& stream,
                                                   http::message<isRequest, Body, Fields>& message)
{
   http::serializer<isRequest, Body, Fields> serializer{message};
   co_return co_await h1::write(stream, serializer);
}

// =================================================================================================

} // namespace anyhttp::beast_impl::h1

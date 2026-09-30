//
// What the CAPY port builds on, checked against the pinned capy/corosio (docs/capy-port-plan.md).
// Should one of these break after moving a pin, it says which assumption went away. Empty in an
// ASIO build.
//
#if ANYHTTP_CAPY

#include <boost/beast/core/buffers_range.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/serializer.hpp>
#include <boost/beast/http/string_body.hpp>

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/write.hpp>

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/test/socket_pair.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <system_error>
#include <vector>

using namespace testing;

namespace capy = boost::capy;
namespace corosio = boost::corosio;
namespace http = boost::beast::http;

// =================================================================================================

namespace
{

//
// Reads from `socket` into `parser` until it has a complete message. put() consumes only part of
// what it is given -- the header first, and the body in a call of its own -- so everything it
// leaves has to be offered again.
//
template <class Parser>
capy::task<std::error_code> read_message(corosio::tcp_socket& socket, Parser& parser)
{
   std::array<char, 4096> buffer;
   while (!parser.is_done())
   {
      auto [ec, n] = co_await socket.read_some(capy::mutable_buffer(buffer.data(), buffer.size()));
      if (ec)
         co_return ec;

      for (const char* data = buffer.data(); n > 0 && !parser.is_done();)
      {
         boost::system::error_code bec;
         const auto used = parser.put(boost::asio::buffer(data, n), bec);
         if (bec == http::error::need_more)
            break;
         if (bec)
            co_return bec;
         data += used, n -= used;
      }
   }
   co_return std::error_code{};
}

//
// Writes what `serializer` produces to `socket`. The buffers next() hands out are valid until
// consume(), so their descriptors are copied out of the visitor and consumed after the write.
//
template <class Serializer>
capy::task<std::error_code> write_message(corosio::tcp_socket& socket, Serializer& serializer)
{
   while (!serializer.is_done())
   {
      boost::system::error_code bec;
      std::vector<capy::const_buffer> buffers;
      serializer.next(bec, [&](boost::system::error_code&, const auto& sequence) {
         for (auto buffer : boost::beast::buffers_range_ref(sequence))
            buffers.emplace_back(buffer.data(), buffer.size());
      });
      if (bec)
         co_return bec;

      auto [ec, n] = co_await capy::write(socket, buffers);
      if (ec)
         co_return ec;
      serializer.consume(n);
   }
   co_return std::error_code{};
}

} // namespace

// =================================================================================================

//
// HTTP/1.1 without Beast's asynchronous operations, which need an ASIO stream: its parser and
// serializer, driven by read and write loops on corosio sockets. A chunked request and a response
// with a Content-Length, so that both framings go through both halves.
//
TEST(Corosio, BeastParserAndSerializer)
{
   corosio::io_context context;
   auto [server, client] = corosio::test::make_socket_pair(context);

   std::error_code server_ec, client_ec;
   http::request_parser<http::string_body> request_parser;
   http::response_parser<http::string_body> response_parser;

   auto serve = [](corosio::tcp_socket& socket, auto& parser,
                   std::error_code& result) -> capy::task<> {
      if ((result = co_await read_message(socket, parser)))
         co_return;

      http::response<http::string_body> response{http::status::ok, 11, parser.get().body()};
      response.prepare_payload();
      http::response_serializer<http::string_body> serializer{response};
      result = co_await write_message(socket, serializer);
   };

   auto request = [](corosio::tcp_socket& socket, auto& parser,
                     std::error_code& result) -> capy::task<> {
      http::request<http::string_body> request{http::verb::post, "/echo", 11, "hello, corosio"};
      request.chunked(true);
      http::request_serializer<http::string_body> serializer{request};
      if ((result = co_await write_message(socket, serializer)))
         co_return;

      result = co_await read_message(socket, parser);
   };

   capy::run_async(context.get_executor())(serve(server, request_parser, server_ec));
   capy::run_async(context.get_executor())(request(client, response_parser, client_ec));
   context.run();

   EXPECT_FALSE(server_ec) << server_ec.message();
   EXPECT_FALSE(client_ec) << client_ec.message();
   EXPECT_TRUE(request_parser.chunked());
   EXPECT_EQ(request_parser.get().target(), "/echo");
   EXPECT_EQ(response_parser.get().result_int(), 200);
   EXPECT_EQ(response_parser.get().body(), "hello, corosio");
}

// -------------------------------------------------------------------------------------------------

//
// Why each API style needs error constants of its own: a Boost.System code survives the conversion
// to std::error_code -- Beast's errors stay comparable across it -- but asio's eof does not become
// capy's. The README's "asio::error::eof at the end of a body" is capy::error::eof in CAPY.
//
TEST(Corosio, ErrorCodeInterop)
{
   const std::error_code partial = boost::system::error_code{http::error::partial_message};
   EXPECT_EQ(partial, std::error_code(boost::system::error_code{http::error::partial_message}));
   EXPECT_EQ(partial.message(), "partial message");

   const std::error_code eof = boost::system::error_code{boost::asio::error::eof};
   EXPECT_NE(eof, capy::cond::eof);
   EXPECT_EQ(std::error_code{capy::error::eof}, capy::cond::eof);
}

// =================================================================================================

#endif // ANYHTTP_CAPY

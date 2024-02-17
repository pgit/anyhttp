#pragma once

#include "common.hpp" // IWYU pragma: keep

#include <boost/asio/awaitable.hpp>
#include <boost/url/urls.hpp>

namespace anyhttp::server
{

// =================================================================================================

struct Config
{
   std::string listen_address = "::";
   uint16_t port = 8080;
};

// =================================================================================================

class Request
{
public:
   class Impl;
   explicit Request(std::unique_ptr<Impl> impl);
   Request(Request&& other) noexcept;
   ~Request();

   const asio::any_io_executor& executor() const;

public:
   using ReadSome = void(boost::system::error_code, std::vector<std::uint8_t>);
   using ReadSomeHandler = asio::any_completion_handler<ReadSome>;

   boost::url_view url() const;
   std::optional<size_t> content_length() const noexcept;

   //
   // https://www.boost.org/doc/libs/1_82_0/doc/html/boost_asio/example/cpp20/operations/callback_wrapper.cpp
   //
   template <boost::asio::completion_token_for<ReadSome> CompletionToken>
   auto async_read_some(CompletionToken&& token)
   {
      return boost::asio::async_initiate<CompletionToken, ReadSome>(
         [&](asio::completion_handler_for<ReadSome> auto handler) { //
            async_read_some_any(std::move(handler));
         },
         token);
   }

private:
   void async_read_some_any(ReadSomeHandler&& handler);

private:
   std::unique_ptr<Impl> m_impl;
};

// -------------------------------------------------------------------------------------------------

class Response
{
public:
   class Impl;
   explicit Response(std::unique_ptr<Impl> impl);
   Response(Response&& other) noexcept;
   ~Response();

   const asio::any_io_executor& executor() const;
   void write_head(unsigned int status_code, Fields headers);

public:
   template <boost::asio::completion_token_for<Write> CompletionToken>
   auto async_write(asio::const_buffer buffer, CompletionToken&& token)
   {
      return boost::asio::async_initiate<CompletionToken, Write>(
         [&](asio::completion_handler_for<Write> auto handler, asio::const_buffer buffer) { //
            async_write_any(std::move(handler), buffer);
         },
         token, buffer);
   }

private:
   void async_write_any(WriteHandler&& handler, asio::const_buffer buffer);

   std::unique_ptr<Impl> m_impl;
};

// =================================================================================================

using RequestHandler = std::function<void(Request, Response)>;
using RequestHandlerCoro = std::function<asio::awaitable<void>(Request, Response)>;

class Server
{
public:
   class Impl;
   Server(asio::any_io_executor executor, Config config);
   ~Server();

   void setRequestHandler(RequestHandler&& handler);
   void setRequestHandlerCoro(RequestHandlerCoro&& handler);

   asio::ip::tcp::endpoint local_endpoint() const;

private:
   std::unique_ptr<Impl> impl;
};

// =================================================================================================

} // namespace anyhttp::server

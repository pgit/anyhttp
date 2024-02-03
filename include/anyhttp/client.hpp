#pragma once

#include "common.hpp" // IWYU pragma: keep

#include <boost/asio/buffer.hpp>
#include <boost/url.hpp>

namespace anyhttp
{
class Session;
}

namespace anyhttp::client
{

// =================================================================================================

struct Config
{
   boost::urls::url url{"http://localhost:8080"};
};

// =================================================================================================

class Response
{
public:
   class Impl;
   explicit Response(std::unique_ptr<Impl> impl);
   Response(Response&& other) noexcept;
   ~Response();

   const asio::any_io_executor& executor() const;

public:
   void write_head(unsigned int status_code, Fields headers);

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
   std::unique_ptr<Impl> m_impl;
};

// -------------------------------------------------------------------------------------------------

class Request
{
public:
   class Impl;
   explicit Request(std::unique_ptr<Impl> impl);
   Request(Request&& other) noexcept;
   ~Request();

   const asio::any_io_executor& executor() const;

public:
   using GetResponse = void(boost::system::error_code, Response);
   using GetResponseHandler = asio::any_completion_handler<GetResponse>;

   template <boost::asio::completion_token_for<GetResponse> CompletionToken>
   auto async_get_response(CompletionToken&& token)
   {
      return boost::asio::async_initiate<CompletionToken, GetResponse>(
         [&](asio::completion_handler_for<GetResponse> auto handler) { //
            async_get_response_any(std::move(handler));
         },
         token);
   }

public:
   using Write = void(boost::system::error_code);
   using WriteHandler = asio::any_completion_handler<Write>;

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
   void async_get_response_any(GetResponseHandler&& handler);

   std::unique_ptr<Impl> m_impl;
};

// =================================================================================================

using Connect = void(boost::system::error_code, Session);
using ConnectHandler = asio::any_completion_handler<Connect>;

class Client
{
public:
   class Impl;
   Client(asio::any_io_executor executor, Config config);
   ~Client();

   const asio::any_io_executor& executor() const;

   template <boost::asio::completion_token_for<Connect> CompletionToken>
   auto async_connect(CompletionToken&& token)
   {
      return boost::asio::async_initiate<CompletionToken, Connect>(
         [&](asio::completion_handler_for<Connect> auto handler) { //
            async_connect_any(std::move(handler));
         },
         token);
   }

   asio::ip::tcp::endpoint local_endpoint() const;

private:
   void async_connect_any(ConnectHandler&& handler);

   std::unique_ptr<Impl> impl;
};

// =================================================================================================

} // namespace anyhttp::client

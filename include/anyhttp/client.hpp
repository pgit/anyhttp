#pragma once

#include "common.hpp" // IWYU pragma: keep

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_allocator.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/ip/basic_endpoint.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <boost/url.hpp>

namespace anyhttp::client
{
struct Config
{
   boost::urls::url url{"http://localhost:8080"};
};

// =================================================================================================

class Response
{
   class Impl;
   std::unique_ptr<Impl> m_impl;

public:
   explicit Response(std::unique_ptr<Impl> impl);
   Response(Response&& other) noexcept;
   ~Response();

   const asio::any_io_executor& executor() const;
   void write_head(unsigned int status_code, Headers headers);

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
   void write_head(unsigned int status_code, Headers headers);

public:
   using GetResponse = void (boost::system::error_code, Response&&);
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
   auto async_write(std::vector<std::uint8_t> buffer, CompletionToken&& token)
   {
      return boost::asio::async_initiate<CompletionToken, Write>(
         [&](asio::completion_handler_for<Write> auto handler, std::vector<uint8_t> buffer) { //
            async_write_any(std::move(handler), std::move(buffer));
         },
         token, std::move(buffer));
   }

private:
   void async_write_any(WriteHandler&& handler, std::vector<std::uint8_t> buffer);
   void async_get_response_any(GetResponseHandler&& handler, Response);

   std::unique_ptr<Impl> m_impl;
};

// =================================================================================================

class Client
{
public:
   Client(asio::any_io_executor executor, Config config);
   ~Client();

   Request submit(boost::urls::url url, Headers headers);
   asio::ip::tcp::endpoint local_endpoint() const;

public:
   class Impl;

private:
   std::unique_ptr<Impl> impl;
};

// =================================================================================================

} // namespace anyhttp::server
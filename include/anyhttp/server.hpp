#pragma once

#include "common.hpp" // IWYU pragma: keep

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_allocator.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/ip/basic_endpoint.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <map>

namespace anyhttp::server
{
namespace asio = boost::asio;

struct Config
{
   std::string listen_address = "::";
   uint16_t port = 8080;
};

using Headers = std::map<std::string, std::string>;

class Request
{
public:
   class Impl;
   explicit Request(std::unique_ptr<Impl> impl);
   Request(Request&& other) noexcept;
   ~Request();

   const asio::any_io_executor& executor() const;

public:
   asio::any_completion_handler<void(std::vector<std::uint8_t>)> m_read_handler;

   //
   // https://www.boost.org/doc/libs/1_82_0/doc/html/boost_asio/example/cpp20/operations/callback_wrapper.cpp
   //
   template <boost::asio::completion_token_for<void(std::vector<std::uint8_t>)> CompletionToken>
   auto async_read_some(CompletionToken&& token)
   {
      return boost::asio::async_initiate<CompletionToken, void(std::vector<std::uint8_t>)>(
         [&](asio::completion_handler_for<void(std::vector<std::uint8_t>)> auto handler) { //
            async_read_some_any(std::move(handler));
         },
         token);
   }

private:
   void
   async_read_some_any(asio::any_completion_handler<void(std::vector<std::uint8_t>)>&& handler);

private:
   std::unique_ptr<Impl> m_impl;
};

class Response
{
public:
   class Impl;
   explicit Response(std::unique_ptr<Impl> impl);
   Response(Response&& other) noexcept;
   ~Response();

   const asio::any_io_executor& executor() const;
   void write_head(unsigned int status_code, Headers headers);

   template <boost::asio::completion_token_for<void(std::vector<std::uint8_t>)> CompletionToken>
   auto async_write(std::vector<std::uint8_t> buffer, CompletionToken&& token)
   {
      return boost::asio::async_initiate<CompletionToken, void()>(
         [&](asio::completion_handler_for<void()> auto handler,
             std::vector<uint8_t> buffer) { //
            async_write_any(std::move(handler), std::move(buffer));
         },
         token, std::move(buffer));
   }

private:
   void async_write_any(asio::any_completion_handler<void()>&& handler,
                        std::vector<std::uint8_t> buffer);

   std::unique_ptr<Impl> m_impl;
};

using RequestHandler = std::function<void(Request, Response)>;
using RequestHandlerCoro = std::function<asio::awaitable<void>(Request, Response)>;

class Server
{
public:
   Server(any_io_executor executor, Config config);
   ~Server();

   void setRequestHandler(RequestHandler&& handler);
   void setRequestHandlerCoro(RequestHandlerCoro&& handler);

   ip::tcp::endpoint local_endpoint() const;

public:
   class Impl;

private:
   std::unique_ptr<Impl> impl;
};

} // namespace anyhttp::server

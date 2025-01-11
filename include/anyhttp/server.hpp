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
   bool use_strand = false;
};

// =================================================================================================

class Request
{
public:
   class Impl;
   explicit Request(std::unique_ptr<Impl> impl);
   Request(Request&& other) noexcept;
   ~Request();

   // const asio::any_io_executor& executor() const;

public:
   boost::url_view url() const;
   std::optional<size_t> content_length() const noexcept;

   //
   // https://www.boost.org/doc/libs/1_85_0/doc/html/boost_asio/example/cpp20/operations/callback_wrapper.cpp
   //
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(ReadSome) CompletionToken>
   auto async_read_some(CompletionToken&& token)
   {
      return boost::asio::async_initiate<CompletionToken, ReadSome>(
         [&](ReadSomeHandler handler) { //
            async_read_some_any(std::move(handler));
         },
         token);
   }

private:
   void async_read_some_any(ReadSomeHandler&& handler);

private:
   std::unique_ptr<Impl> impl;
};

// -------------------------------------------------------------------------------------------------

class Response
{
public:
   class Impl;
   explicit Response(std::unique_ptr<Impl> impl);
   Response(Response&& other) noexcept;
   ~Response();

   void content_length(std::optional<size_t> content_length);

public:
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Write) CompletionToken>
   auto async_submit(unsigned int status_code, Fields headers, CompletionToken&& token)
   {
      return boost::asio::async_initiate<CompletionToken, Write>(
         [this](WriteHandler handler, unsigned int status_code, Fields headers) { //
            async_submit_any(std::move(handler), status_code, headers);
         },
         token, status_code, headers);
   }

   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Write) CompletionToken>
   auto async_write(asio::const_buffer buffer, CompletionToken&& token)
   {
      return boost::asio::async_initiate<CompletionToken, Write>(
         [this](WriteHandler handler, asio::const_buffer buffer) { //
            async_write_any(std::move(handler), buffer);
         },
         token, buffer);
   }

private:
   void async_submit_any(WriteHandler&& handler, unsigned int status_code, Fields headers);
   void async_write_any(WriteHandler&& handler, asio::const_buffer buffer);

   std::unique_ptr<Impl> impl;
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

   Server(const Server& other) = delete;
   Server(Server&& other) = default;
   Server& operator=(const Server& other) = delete;
   Server& operator=(Server&& other) = default;

   const asio::any_io_executor& executor() const;
   void setRequestHandler(RequestHandler&& handler);
   void setRequestHandlerCoro(RequestHandlerCoro&& handler);

   asio::ip::tcp::endpoint local_endpoint() const;

private:
   std::shared_ptr<Impl> impl;
};

// =================================================================================================

} // namespace anyhttp::server

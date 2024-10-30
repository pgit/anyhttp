#pragma once

#include "common.hpp" // IWYU pragma: keep

#include <boost/asio/buffer.hpp>
#include <boost/url.hpp>

namespace anyhttp
{
class Session;
namespace client
{

// =================================================================================================

struct Config
{
   // FIXME: the client does not connect to an URL, it connects to a host:port or endpoint
   boost::urls::url url{"localhost:8080"};
   Protocol protocol{Protocol::http2};
};

// =================================================================================================

class Response
{
public:
   class Impl;
   explicit Response(std::unique_ptr<Impl> impl);
   Response(Response&& other) noexcept;
   ~Response();

public:
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

   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(GetResponse) CompletionToken>
   auto async_get_response(CompletionToken&& token)
   {
      return boost::asio::async_initiate<CompletionToken, GetResponse>(
         [&](GetResponseHandler handler) { //
            async_get_response_any(std::move(handler));
         },
         std::forward<CompletionToken>(token));
   }

public:
   using Write = void(boost::system::error_code);
   using WriteHandler = asio::any_completion_handler<Write>;

   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Write) CompletionToken>
   auto async_write(asio::const_buffer buffer, CompletionToken&& token)
   {
      return boost::asio::async_initiate<CompletionToken, Write>(
         [&](WriteHandler handler, asio::const_buffer buffer) { //
#if 0
            //
            // https://stackoverflow.com/questions/76004637/asio-awaitable-operator-dont-return-when-timer-expires
            //
            auto cs = asio::get_associated_cancellation_slot(handler);
            auto work = make_work_guard(asio::get_associated_executor(handler));
            cs.assign(
               [work = std::move(work)](asio::cancellation_type type) mutable
               {
                  using ct = asio::cancellation_type;
                  if (ct::none != (type & ct::terminal))
                     loge("cancellation_type terminal");
                  if (ct::none != (type & ct::partial))
                     loge("cancellation_type partial");
                  if (ct::none != (type & ct::total))
                     loge("cancellation_type total");

                  work.reset();
               });
#endif
            async_write_any(std::move(handler), buffer);
         },
         std::forward<CompletionToken>(token), buffer);
   }

private:
   void async_write_any(WriteHandler handler, asio::const_buffer buffer);
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

   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Connect) CompletionToken>
   auto async_connect(CompletionToken&& token)
   {
      return boost::asio::async_initiate<CompletionToken, Connect>(
         [&](ConnectHandler&& handler) { //
            async_connect_any(std::move(handler));
         },
         token);
   }

private:
   void async_connect_any(ConnectHandler&& handler);

   std::unique_ptr<Impl> impl;
};

// =================================================================================================

} // namespace client
} // namespace anyhttp

#pragma once

#include "common.hpp" // IWYU pragma: keep

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/bind_allocator.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/ip/basic_endpoint.hpp>
#include <boost/asio/ip/tcp.hpp>

namespace anyhttp::server
{
namespace asio = boost::asio;

struct Config
{
   std::string listen_address = "::";
   uint16_t port = 8080;
};

class Request
{
public:
   ~Request();

   asio::any_completion_handler<void(std::vector<std::uint8_t>)> m_read_handler;

   //
   // https://www.boost.org/doc/libs/1_82_0/doc/html/boost_asio/example/cpp20/operations/callback_wrapper.cpp
   //
   template <boost::asio::completion_token_for<void(std::vector<std::uint8_t>)> CompletionToken>
   auto async_read_some(CompletionToken&& token)
   {
      assert(!m_read_handler);

      auto init = [&](asio::completion_handler_for<void(std::vector<std::uint8_t>)> auto handler)
      {
         auto work = boost::asio::make_work_guard(handler);

         asio::any_completion_handler<void(std::vector<std::uint8_t>)> test = handler;

         assert(!m_read_handler);

         m_read_handler = [handler = std::move(handler),
                           work = std::move(work)](std::vector<std::uint8_t> result) mutable
         {
            auto alloc = boost::asio::get_associated_allocator(
               handler, boost::asio::recycling_allocator<void>());

            boost::asio::dispatch(
               work.get_executor(),
               asio::bind_allocator(alloc, [handler = std::move(handler),
                                            result = std::move(result)]() mutable { //
                  std::move(handler)(result);
               }));
         };

         // call_handler_loop();
      };

      return boost::asio::async_initiate<CompletionToken, void(std::vector<std::uint8_t>)>(init,
                                                                                           token);
   }

private:
   class Impl;
   std::shared_ptr<Impl> impl;
};

class Response
{
public:
   ~Response();

private:
   class Impl;
   std::shared_ptr<Impl> impl;
};

using RequestHandler = std::function<void(Request, Response)>;

class Server
{
public:
   Server(any_io_executor executor, Config config);
   ~Server();

   void setRequestHandler(RequestHandler&& handler);

   ip::tcp::endpoint local_endpoint() const;

public:
   class Impl;

private:
   std::unique_ptr<Impl> impl;
};

} // namespace anyhttp::server

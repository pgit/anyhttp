#pragma once
#include "client.hpp"

#include <boost/asio.hpp>
#include <boost/asio/any_completion_handler.hpp>

namespace anyhttp
{
class Session;
}

namespace anyhttp::client
{

// =================================================================================================

class Request::Impl
{
public:
   Impl() noexcept;
   virtual ~Impl();

   virtual const asio::any_io_executor& executor() const = 0;
   virtual void async_submit(WriteHandler&& handler, unsigned int status_code, Fields headers) = 0;
   virtual void async_write(WriteHandler&& handler, asio::const_buffer buffer) = 0;
   virtual void async_get_response(GetResponseHandler&& handler) = 0;
   virtual void detach() = 0;
   virtual void destroy(std::unique_ptr<Impl> self) { /* delete self */ }
};

// -------------------------------------------------------------------------------------------------

class Response::Impl
{
public:
   Impl() noexcept;
   virtual ~Impl();

   virtual const asio::any_io_executor& executor() const = 0;
   virtual boost::url_view url() const = 0;
   virtual std::optional<size_t> content_length() const noexcept = 0;
   virtual void async_read_some(ReadSomeHandler&& handler) = 0;
   virtual void detach() = 0;
   virtual void destroy(std::unique_ptr<Impl> self) { /* delete self */ }
};

// =================================================================================================

class Client::Impl
{
public:
   explicit Impl(boost::asio::any_io_executor executor, Config config);
   ~Impl();

   const Config& config() const { return m_config; }
   const boost::asio::any_io_executor& executor() const { return m_executor; }

   asio::awaitable<void> connect(ConnectHandler handler);
   void async_connect(ConnectHandler&& handler);

   asio::ip::tcp::endpoint local_endpoint() const { return m_acceptor.local_endpoint(); }

private:
   Config m_config;
   boost::asio::any_io_executor m_executor;
   asio::ip::tcp::acceptor m_acceptor;
};

// =================================================================================================

} // namespace anyhttp::client
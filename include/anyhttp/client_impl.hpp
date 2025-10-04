#pragma once
#include "client.hpp"

#include <boost/asio.hpp>
#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/experimental/co_composed.hpp>

namespace anyhttp::client
{

// =================================================================================================

class Request::Impl : public impl::Writer
{
public:
   Impl() noexcept;
   virtual ~Impl();

   virtual void async_submit(WriteHandler&& handler, unsigned int status_code, const Fields& headers) = 0;
   virtual void async_get_response(GetResponseHandler&& handler) = 0;

   using ReaderOrWriter = impl::Writer;
};

// -------------------------------------------------------------------------------------------------

class Response::Impl : public impl::Reader
{
public:
   Impl() noexcept;
   virtual ~Impl();

   virtual unsigned int status_code() const noexcept = 0;
   virtual boost::url_view url() const = 0;

   using ReaderOrWriter = impl::Reader;
};

// =================================================================================================

class Client::Impl
{
public:
   explicit Impl(asio::any_io_executor executor, Config config);
   ~Impl();

   boost::asio::any_io_executor get_executor() const noexcept { return m_executor; }

   void async_connect(ConnectHandler handler);

private:
   const Config& config() const { return m_config; }
   awaitable<Session> async_connect();

private:
   Config m_config;
   asio::any_io_executor m_executor;
   std::optional<asio::ip::tcp::resolver> m_resolver;
};

// =================================================================================================

} // namespace anyhttp::client


#include <boost/asio/error.hpp>
#include <utility>

#include "anyhttp/client.hpp"
#include "anyhttp/client_impl.hpp"

namespace anyhttp
{
std::string to_string(Protocol protocol)
{
   switch (protocol)
   {
   case Protocol::http11:
      return "HTTP11";
   case Protocol::http2:
      return "HTTP2";
   default:
      return fmt::format("UNKNOWN ({})", std::to_underlying(protocol));
   }
}

} // namespace anyhttp

// =================================================================================================

namespace anyhttp::client
{

Request::Request(std::unique_ptr<Request::Impl> impl) : m_impl(std::move(impl))
{
   logd("\x1b[1;34mClient::Request: ctor\x1b[0m");
}
Request::Request(Request&&) noexcept = default;
Request::~Request()
{
   if (!m_impl) // may happen if moved-from
      return;

   logd("\x1b[34mClient::Request: dtor\x1b[0m");
   auto& impl = *m_impl;
   impl.destroy(std::move(m_impl)); // give implementation a chance for cancellation
   assert(!m_impl);
}

void Request::async_write_any(WriteHandler&& handler, asio::const_buffer buffer)
{
   if (!m_impl)
   {
      std::move(handler)(boost::asio::error::operation_aborted);
      return;
   }

   m_impl->async_write(std::move(handler), buffer);
}

void Request::async_get_response_any(Request::GetResponseHandler&& handler)
{
   if (!m_impl)
   {
      std::move(handler)(boost::asio::error::operation_aborted, Response{nullptr});
      return;
   }

   m_impl->async_get_response(std::move(handler));
}

const asio::any_io_executor& Request::executor() const
{
   assert(m_impl);
   return m_impl->executor();
}

// -------------------------------------------------------------------------------------------------

Response::Response(std::unique_ptr<Response::Impl> impl) : m_impl(std::move(impl))
{
   logd("\x1b[1;34mClient::Response: ctor\x1b[0m");
}
Response::Response(Response&&) noexcept = default;
Response::~Response()
{
   if (!m_impl)
      return;

   logd("\x1b[34mClient::Response: dtor\x1b[0m");
   auto& impl = *m_impl;
   impl.destroy(std::move(m_impl)); // give implementation a chance for cancellation
}

void Response::async_read_some_any(ReadSomeHandler&& handler)
{
   assert(m_impl);
   m_impl->async_read_some(std::move(handler));
}

// =================================================================================================

Client::Client(boost::asio::any_io_executor executor, Config config)
   : impl(std::make_unique<Client::Impl>(std::move(executor), std::move(config)))
{
}

Client::~Client() = default;

void Client::async_connect_any(ConnectHandler&& handler)
{
   impl->async_connect(std::move(handler));
}

const asio::any_io_executor& Client::executor() const { return impl->executor(); }
asio::ip::tcp::endpoint Client::local_endpoint() const { return impl->local_endpoint(); }

// =================================================================================================

} // namespace anyhttp::client

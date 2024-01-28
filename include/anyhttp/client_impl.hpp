#pragma once
#include "client.hpp"
#include "session.hpp"

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
   virtual void write_head(unsigned int status_code, Fields headers) = 0;
   virtual void async_write(WriteHandler&& handler, std::vector<uint8_t> bufffer) = 0;
   virtual void detach() = 0;
};

// -------------------------------------------------------------------------------------------------

class Response::Impl
{
public:
   Impl() noexcept;
   virtual ~Impl();

   virtual const asio::any_io_executor& executor() const = 0;
   virtual void async_read_some(ReadSomeHandler&& handler) = 0;
   virtual void detach() = 0;
};

// =================================================================================================

class Client::Impl
{
public:
   explicit Impl(boost::asio::any_io_executor executor, Config config);
   ~Impl();

   const Config& config() const { return m_config; }
   const boost::asio::any_io_executor& executor() const { return m_executor; }

   asio::awaitable<void> connect();
   Request submit(boost::urls::url url, Fields headers);
   asio::ip::tcp::endpoint local_endpoint() const { return m_acceptor.local_endpoint(); }

   void run();

private:
   Config m_config;
   boost::asio::any_io_executor m_executor;
   asio::ip::tcp::acceptor m_acceptor;
   std::shared_ptr<Session::Impl> m_session;
};

// =================================================================================================

} // namespace anyhttp::client
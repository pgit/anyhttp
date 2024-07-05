#pragma once

#include "common.hpp" // IWYU pragma: keep

#include "client_impl.hpp"
#include "server_impl.hpp"
#include "session_impl.hpp"

#include <boost/asio.hpp>

#include <boost/asio/buffer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/serializer.hpp>
#include <boost/beast/http/buffer_body.hpp>

using namespace boost::asio;

namespace anyhttp::beast_impl
{

// =================================================================================================

using stream = asio::as_tuple_t<asio::deferred_t>::as_default_on_t<asio::ip::tcp::socket>;

class BeastSession : public ::anyhttp::Session::Impl
{
   BeastSession(std::string_view logPrefix, any_io_executor executor, ip::tcp::socket&& socket);

public:
   BeastSession(server::Server::Impl& parent, any_io_executor executor, ip::tcp::socket&& socket);
   BeastSession(client::Client::Impl& parent, any_io_executor executor, ip::tcp::socket&& socket);
   ~BeastSession() override;
   void destroy() override;

   // ----------------------------------------------------------------------------------------------

   void async_submit(SubmitHandler&& handler, boost::urls::url url, Fields headers) override;
   
   awaitable<void> do_server_session(Buffer&& data) override;
   awaitable<void> do_client_session(Buffer&& data) override;

   // ----------------------------------------------------------------------------------------------

   void async_read_some(server::Request::ReadSomeHandler&& handler);
   void write_head(unsigned int status_code, Fields headers);
   void async_write(WriteHandler&& handler, asio::const_buffer buffer);

   // ----------------------------------------------------------------------------------------------

   server::Server::Impl& server()
   {
      assert(m_server);
      return *m_server;
   }

   client::Client::Impl& client()
   {
      assert(m_client);
      return *m_client;
   }

   const auto& executor() const { return m_executor; }
   const std::string& logPrefix() const { return m_logPrefix; }

public:
   boost::urls::url url;
   boost::beast::tcp_stream m_stream;
   Buffer m_buffer;

private:
   server::Server::Impl* m_server = nullptr;
   client::Client::Impl* m_client = nullptr;
   asio::any_io_executor m_executor;
   std::string m_logPrefix;
};

// =================================================================================================

} // namespace anyhttp::beast_impl
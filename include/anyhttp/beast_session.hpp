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
namespace http = boost::beast::http;

namespace anyhttp::beast_impl
{

// =================================================================================================

template<bool IsServer>
struct Stream;

class BeastSession;
class BeastReader : public server::Request::Impl, public client::Response::Impl
{
public:
   explicit BeastReader(BeastSession& session);
   ~BeastReader() override;
   void detach() override;

   boost::url_view url() const override;
   void async_read_some(server::Request::ReadSomeHandler&& handler) override;
   const asio::any_io_executor& executor() const override;

   Stream<false>* stream;
   BeastSession* session;
};

// -------------------------------------------------------------------------------------------------

class BeastWriter : public server::Response::Impl, public client::Request::Impl
{
public:
   explicit BeastWriter(BeastSession& session);
   ~BeastWriter() override;
   void detach() override;

   void write_head(unsigned int status_code, Fields headers) override;
   void async_write(WriteHandler&& handler, asio::const_buffer buffer) override;
   void async_get_response(client::Request::GetResponseHandler&& handler) override;
   const asio::any_io_executor& executor() const override;

   BeastSession* session;
};

// =================================================================================================

using stream = asio::as_tuple_t<asio::deferred_t>::as_default_on_t<asio::ip::tcp::socket>;

class BeastSession : public ::anyhttp::Session::Impl
{
   BeastSession(any_io_executor executor, ip::tcp::socket&& socket);

public:
   BeastSession(server::Server::Impl& parent, any_io_executor executor, ip::tcp::socket&& socket);
   BeastSession(client::Client::Impl& parent, any_io_executor executor, ip::tcp::socket&& socket);
   ~BeastSession() override;

   // ----------------------------------------------------------------------------------------------

   client::Request submit(boost::urls::url url, Fields headers) override;
   awaitable<void> do_server_session(std::vector<uint8_t> data) override;
   awaitable<void> do_client_session(std::vector<uint8_t> data) override;
   void cancel() override
   {
      boost::system::error_code ec;
      std::ignore = m_stream.socket().shutdown(boost::asio::socket_base::shutdown_both, ec);
      logw("[{}] shutdown: {}", m_logPrefix, ec.message());
   }

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
   std::vector<uint8_t> m_data;
   decltype(dynamic_buffer(m_data)) m_buffer{m_data};

   std::vector<uint8_t> request_buffer;
   http::request_parser<boost::beast::http::buffer_body> request_parser;
   http::response<http::buffer_body> response;  
   http::response_serializer<http::buffer_body> response_serializer{response};
   
   http::request<http::buffer_body> request; 
   http::request_serializer<http::buffer_body> request_serializer{request};
   http::response_parser<boost::beast::http::buffer_body> response_parser;

private:
   server::Server::Impl* m_server = nullptr;
   client::Client::Impl* m_client = nullptr;
   asio::any_io_executor m_executor;
   std::string m_logPrefix;

   std::vector<uint8_t> m_send_buffer;

   size_t m_requestCounter = 0;
};

// =================================================================================================

} // namespace anyhttp::beast_impl
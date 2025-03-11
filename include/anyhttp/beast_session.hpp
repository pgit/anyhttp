#pragma once

#include "common.hpp" // IWYU pragma: keep

#include "client_impl.hpp"
#include "server_impl.hpp"
#include "session_impl.hpp"

#include <boost/asio.hpp>

#include <boost/asio/buffer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/serializer.hpp>

using namespace boost::asio;

namespace anyhttp::beast_impl
{

// =================================================================================================

template <typename Stream>
class BeastSession : public ::anyhttp::Session::Impl
{
   using stream_type = Stream;

protected:
   BeastSession(std::string_view logPrefix, any_io_executor executor, stream_type&& stream);

public:
   ~BeastSession() override;

   const std::string& logPrefix() const { return m_logPrefix; }
   const auto& executor() const { return m_executor; }

   // ----------------------------------------------------------------------------------------------

   void async_read_some(ReadSomeHandler&& handler);
   void async_write(WriteHandler&& handler, asio::const_buffer buffer);

   // ----------------------------------------------------------------------------------------------

   void destroy(std::shared_ptr<Session::Impl>) override;

   // ----------------------------------------------------------------------------------------------

public:
   std::string m_logPrefix;
   asio::any_io_executor m_executor;
   stream_type m_stream;
   Buffer m_buffer;
};

// =================================================================================================

class ServerSessionBase
{
public:
   inline ServerSessionBase(server::Server::Impl& parent) : m_server(&parent) {}
   server::Server::Impl& server()
   {
      assert(m_server);
      return *m_server;
   }

private:
   server::Server::Impl* m_server = nullptr;
};

template <typename Stream>
class ServerSession : public ServerSessionBase, public BeastSession<Stream>
{
   using super = BeastSession<Stream>;

   // FIXME: maybe use CRTP or something similar to avoid this?
   using super::logPrefix;
   using super::m_buffer;
   using super::m_stream;

public:
   ServerSession(server::Server::Impl& parent, any_io_executor executor, Stream&& stream);

   void async_submit(SubmitHandler&& handler, boost::urls::url url, Fields headers) override;
   awaitable<void> do_session(Buffer&& data) override;
};

// -------------------------------------------------------------------------------------------------

class ClientSessionBase
{
public:
   inline ClientSessionBase(client::Client::Impl& parent) : m_client(&parent) {}
   client::Client::Impl& client()
   {
      assert(m_client);
      return *m_client;
   }

private:
   client::Client::Impl* m_client = nullptr;
};

template <typename Stream>
class ClientSession : public ClientSessionBase, public BeastSession<Stream>
{
   using super = BeastSession<Stream>;

   // FIXME: maybe use CRTP or something similar to avoid this?
   using super::logPrefix;
   using super::m_buffer;
   using super::m_stream;

public:
   ClientSession(client::Client::Impl& parent, any_io_executor executor, Stream&& stream);

   void async_submit(SubmitHandler&& handler, boost::urls::url url, Fields headers) override;
   awaitable<void> do_session(Buffer&& data) override;

   client::Client::Impl& client()
   {
      assert(m_client);
      return *m_client;
   }

private:
   client::Client::Impl* m_client = nullptr;
};

// =================================================================================================

} // namespace anyhttp::beast_impl
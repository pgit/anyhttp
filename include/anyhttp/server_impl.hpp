#pragma once
#include "server.hpp"
#include "session.hpp"

#include <boost/asio.hpp>
#include <boost/asio/any_completion_handler.hpp>

#include <memory>
#include <set>
#include <unordered_map>

// Forward declaration so we don't drag <ngtcp2/ngtcp2.h> into every translation unit.
struct ngtcp2_cid;

namespace anyhttp
{
class Session;
}

namespace anyhttp::server
{

// =================================================================================================

class Request::Impl : public impl::Reader
{
public:
   Impl() noexcept;
   virtual ~Impl();

   // FIXME: doesn't make sense to have a status_code() for a server request, but keeps beast happy
   virtual unsigned int status_code() const noexcept = 0;
   virtual boost::url_view url() const = 0;

   using ReaderOrWriter = impl::Reader;
};

// -------------------------------------------------------------------------------------------------

class Response::Impl : public impl::Writer
{
public:
   Impl() noexcept;
   virtual ~Impl();

   virtual void async_submit(StatusHandler&& handler, unsigned int status_code,
                             const Fields& fields) = 0;

   using ReaderOrWriter = impl::Writer;
};

// =================================================================================================

struct Endpoint;
class Http3Session;
struct QuicBatch;

class Server::Impl : public std::enable_shared_from_this<Server::Impl>
{
public:
   Impl(boost::asio::any_io_executor executor, Config config);
   ~Impl();

   void start();
   void destroy();

   void listen_tcp();
   void listen_udp();

   const Config& config() const { return m_config; }
   boost::asio::any_io_executor get_executor() const noexcept { return m_executor; }

   asio::awaitable<void> tcp_listen_loop();
   asio::awaitable<void> handleConnection(asio::ip::tcp::socket socket);

   asio::ip::tcp::endpoint local_endpoint() const
   {
      assert(m_acceptor);
      return m_acceptor->local_endpoint();
   }

   void setRequestHandler(RequestHandler&& handler) { m_requestHandler = std::move(handler); }
   const RequestHandler& requestHandler() const { return m_requestHandler; }

   asio::awaitable<void> udp_receive_loop();
   int udp_on_read(Endpoint& ep);
   void process_quic_batch(const std::shared_ptr<Http3Session>& session, QuicBatch&& batch);

   //
   // QUIC connection-ID demux table. Populated by QuicHandler as new source CIDs are minted,
   // consulted by udp_on_read() to route packets to the right connection. Guarded by
   // m_quicMutex: the receive loop reads it while sessions mutate it from their own strands
   // (get_new_connection_id/remove_connection_id callbacks, close timers).
   //
   void associate_quic_cid(const ngtcp2_cid& cid, Http3Session* session);
   void dissociate_quic_cid(const ngtcp2_cid& cid);
   void erase_quic_session(Http3Session* h);

private:
   Config m_config;

   boost::asio::any_io_executor m_executor;
   std::optional<asio::ip::tcp::acceptor> m_acceptor;
   std::optional<asio::ip::udp::socket> m_udp_socket;

   std::mutex m_sessionMutex;
   std::set<std::shared_ptr<Session::Impl>> m_sessions;

   std::mutex m_quicMutex;
   std::unordered_map<std::string, std::shared_ptr<Http3Session>> m_quic_handlers;

   RequestHandler m_requestHandler;
   bool m_destroyed = false;
};

// =================================================================================================

} // namespace anyhttp::server
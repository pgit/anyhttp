#pragma once

//
// The HTTP/2 session, nghttp2 version 2. The counterpart of h2/session.hpp, with the same
// definitions in the same order, so that the two can be read side by side -- see H2_LIBRARY.
//
// Version 2 is shaped like nghttp3: one nghttp2_conn instead of a session plus an option object,
// settings as a plain struct, callbacks as a plain struct, body data taken by reference through a
// data reader, flow control credit extended by hand, and a connection-level expiry timer.
//

#include "anyhttp/client_impl.hpp"
#include "anyhttp/common.hpp"
#include "anyhttp/h2/backend.hpp"
#include "anyhttp/h2v2/stream.hpp"
#include "anyhttp/server_impl.hpp"
#include "anyhttp/session_impl.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/beast/core/flat_buffer.hpp>

#include <map>
#include <optional>

#include <nghttp2v2/nghttp2.h>

namespace anyhttp::nghttp2
{

// =================================================================================================

//
// Version 1's nghttp2_unique_ptr and NGHTTP2_NEW have no counterpart here: nghttp2_settings and
// nghttp2_callbacks are plain structs the application fills in and nghttp2_conn_server_new()
// copies, so there is nothing to own but the connection itself.
//

// =================================================================================================

class NGHttp2Stream;

class NGHttp2Session : public anyhttp::Session::Impl
{
public:
   NGHttp2Session(std::string_view prefix, Executor executor);
   virtual ~NGHttp2Session();

   Executor get_executor() const noexcept override { return executor_; }
   const std::string& log_prefix() const { return log_prefix_; }

   std::string log_prefix(int64_t stream_id) const
   {
      if (stream_id)
         return std::format("{}.{}", log_prefix(), stream_id);
      else
         return log_prefix();
   }

   // ----------------------------------------------------------------------------------------------

   void async_submit(SubmitHandler&& handler, std::string_view method, boost::urls::url url,
                     const Fields& headers) override;

   // ----------------------------------------------------------------------------------------------

   // Set by `start_write()`: nghttp2 may have something to send. The send loop waits for it once
   // it has run out of data to send.
   Event send_ready_;

   // Set by `recv_loop()` when it ends: nothing more comes in, so the send loop ends as soon as it
   // has run out of data to send, whether nghttp2 still wants to read or not.
   bool recv_done_ = false;

   //
   // Set once nghttp2_conn_write() has reported NGHTTP2_ERR_CLOSING, which is version 2's way of
   // saying the connection is finished -- version 1 had want_read() and want_write() instead. From
   // then on nothing more may be asked of \c conn, so both loops end.
   //
   bool closing_ = false;

   // Set once a GOAWAY of ours has gone out. Version 1 saw it in on_frame_send_callback(); version
   // 2 has no frame-level send callback, so this is set where the GOAWAY is asked for.
   bool goaway_sent_ = false;

   void start_write();

   // ----------------------------------------------------------------------------------------------

   /**
    * Helper function to pass data from #buffer_ to nghttp2, invoked by recv_loop().
    * The buffer will be empty when this function returns. Terminates the session on error.
    */
   void handle_buffer_contents();

   virtual Task<void> send_loop() = 0;
   virtual Task<void> recv_loop() = 0;

   // ----------------------------------------------------------------------------------------------

   nghttp2_callbacks setup_callbacks();

   /**
    * Fills in what both roles set the same way, see server_settings() / client_settings(). The
    * caller adds what is specific to its role.
    */
   void setup_settings(nghttp2_settings& settings);

   /**
    * Called with the value of an "Alt-Svc" received from the peer as a response header field. A
    * server has nothing to do with one, so this does nothing unless the session is a client's.
    *
    * Version 2 has no ALTSVC extension frame, so a response header field is the only way one
    * arrives -- version 1 also accepted the frame, see its ClientSession::do_session().
    */
   virtual void on_alt_svc(std::string_view field_value) {}

   NGHttp2Stream* create_stream(int64_t stream_id);
   NGHttp2Stream* find_stream(int64_t stream_id);
   void close_stream(int64_t stream_id);
   void delete_stream(int64_t stream_id);

   // ----------------------------------------------------------------------------------------------

   //
   // The connection-level timer, as in QuicSession: nghttp2_conn_get_expiry() says when the
   // library wants to be called back, nghttp2_conn_handle_expiry() does the work. It drives the
   // SETTINGS acknowledgement timeout and the "glitch" rate limiter.
   //
   void arm_timer_from_nghttp2();
   void handle_expiry();

public:
   std::string log_prefix_;
   Executor executor_;

   nghttp2_conn* conn = nullptr;
   std::map<int64_t, std::shared_ptr<NGHttp2Stream>> streams_;
   int64_t last_id_ = 0;
   size_t request_counter_ = 0;

   /// The largest header section accepted from the peer, see Config::max_header_size.
   size_t max_header_size_ = default_max_header_size;

   /// How many requests the peer may have in progress, see Config::max_concurrent_streams.
   uint32_t max_concurrent_streams_ = 100;

   //
   // What to advertise as this origin's HTTP/3 endpoint in every response, see
   // server::Config::alt_svc_max_age. Only a server session ever has one.
   //
   std::string alt_svc_;

   Buffer buffer_;
   Timer timer_;
};

// -------------------------------------------------------------------------------------------------

template <typename Stream>
class NGHttp2SessionImpl : public NGHttp2Session
{
protected:
   NGHttp2SessionImpl(std::string_view log_prefix, Executor executor, Stream&& stream)
      : NGHttp2Session(log_prefix, executor), stream_(std::move(stream))
   {
   }

public:
   Task<void> send_loop() override;
   Task<void> recv_loop() override;
   void destroy() noexcept override;

public:
   Stream stream_;
};

// =================================================================================================

class ServerReference
{
public:
   explicit ServerReference(server::Server::Impl& parent) : server_(&parent) {}
   server::Server::Impl& server()
   {
      assert(server_);
      return *server_;
   }

private:
   server::Server::Impl* server_ = nullptr;
};

// -------------------------------------------------------------------------------------------------

template <typename Stream>
class ServerSession : public ServerReference, public NGHttp2SessionImpl<Stream>
{
   using super = NGHttp2SessionImpl<Stream>;

   // FIXME: maybe use CRTP or something similar to avoid this?
   using super::handle_buffer_contents;
   using super::log_prefix;
   using super::recv_loop;
   using super::send_loop;

   using super::alt_svc_;
   using super::buffer_;
   using super::conn;
   using super::max_concurrent_streams_;
   using super::max_header_size_;
   using super::stream_;

public:
   ServerSession(server::Server::Impl& parent, Executor executor, Stream&& stream);

   Task<void> do_session(Buffer&& data) override;

   /// Set if this session continues an HTTP/1.1 request that has been upgraded to h2c.
   std::optional<Upgrade> upgrade_;
};

// =================================================================================================

class ClientReference
{
public:
   explicit ClientReference(client::Client::Impl& parent) : client_(&parent) {}
   client::Client::Impl& client()
   {
      assert(client_);
      return *client_;
   }

private:
   client::Client::Impl* client_ = nullptr;
};

// -------------------------------------------------------------------------------------------------

template <typename Stream>
class ClientSession : public ClientReference, public NGHttp2SessionImpl<Stream>
{
   using super = NGHttp2SessionImpl<Stream>;

   // FIXME: maybe use CRTP or something similar to avoid this?
   using super::handle_buffer_contents;
   using super::log_prefix;
   using super::recv_loop;
   using super::send_loop;

   using super::buffer_;
   using super::conn;
   using super::max_header_size_;
   using super::stream_;

public:
   ClientSession(client::Client::Impl& parent, Executor executor, Stream&& stream);

   Task<void> do_session(Buffer&& data) override;

   void on_alt_svc(std::string_view field_value) override { client().on_alt_svc(field_value); }
};

// =================================================================================================

} // namespace anyhttp::nghttp2

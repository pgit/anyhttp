#pragma once

#include "anyhttp/common.hpp"
#include "client_impl.hpp"
#include "h2_backend.hpp"
#include "h2_stream.hpp"
#include "server_impl.hpp"
#include "session_impl.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/stream_traits.hpp>

#include <map>
#include <optional>

#include "nghttp2/nghttp2.h"

namespace anyhttp::nghttp2
{

// =================================================================================================

/// RAII wrapper for NGHTTP2 objects, wrapping them in a std::unique_ptr with a custom deleter.
template <class T>
using nghttp2_unique_ptr = std::unique_ptr<T, void (*)(T*)>;

#define NGHTTP2_NEW(X)                                                                             \
   static nghttp2_unique_ptr<nghttp2_##X> nghttp2_##X##_new()                                      \
   {                                                                                               \
      nghttp2_##X* ptr;                                                                            \
      if (nghttp2_##X##_new(&ptr))                                                                 \
         throw std::runtime_error("nghttp2_" #X "_new");                                           \
      return {ptr, nghttp2_##X##_del};                                                             \
   }

NGHTTP2_NEW(session_callbacks)
NGHTTP2_NEW(option)

// =================================================================================================

class NGHttp2Stream;

class NGHttp2Session : public anyhttp::Session::Impl
{
public:
   NGHttp2Session(std::string_view prefix, asio::any_io_executor executor);
   virtual ~NGHttp2Session();

   asio::any_io_executor get_executor() const noexcept override { return executor_; }
   const std::string& log_prefix() const { return log_prefix_; }

   std::string log_prefix(int stream_id) const
   {
      if (stream_id)
         return std::format("{}.{}", log_prefix(), stream_id);
      else
         return log_prefix();
   }

   std::string log_prefix(const nghttp2_frame* frame) const
   {
      return log_prefix(frame->hd.stream_id);
   }

   // ----------------------------------------------------------------------------------------------

   void async_submit(SubmitHandler&& handler, std::string_view method, boost::urls::url url,
                     const Fields& headers) override;

   // ----------------------------------------------------------------------------------------------

   using Resume = void();
   using ResumeHandler = asio::any_completion_handler<Resume>;

   // If set, the send loop has run out of data to send and is waiting for re-activation.
   ResumeHandler send_handler_;

   // Wait to be resumed via `start_write()`, called from within `send_loop()`.
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Resume) CompletionToken = DefaultCompletionToken>
   auto async_wait_send(CompletionToken&& token = CompletionToken())
   {
      return asio::async_initiate<CompletionToken, Resume>(
         [&](ResumeHandler handler) {
            assert(!send_handler_);
            send_handler_ = std::move(handler);
         },
         std::forward<CompletionToken>(token));
   }

   void start_write();

   // ----------------------------------------------------------------------------------------------

   /**
    * Helper function to pass data from #buffer_ to nghttp2, invoked by recv_loop().
    * The buffer will be empty when this function returns. Terminates the session on error.
    */
   void handle_buffer_contents();

   virtual awaitable<void> send_loop() = 0;
   virtual awaitable<void> recv_loop() = 0;

   // ----------------------------------------------------------------------------------------------

   nghttp2_unique_ptr<nghttp2_session_callbacks> setup_callbacks();

   /**
    * Called with the value of an "Alt-Svc" received from the peer, either as a response header
    * field or as an ALTSVC frame (RFC 7838). A server has nothing to do with one, so this does
    * nothing unless the session is a client's, see ClientSession.
    */
   virtual void on_alt_svc(std::string_view field_value) {}

   NGHttp2Stream* create_stream(int32_t stream_id);
   NGHttp2Stream* find_stream(int32_t stream_id);
   void close_stream(int32_t stream_id);
   void delete_stream(int32_t stream_id);

public:
   std::string log_prefix_;
   asio::any_io_executor executor_;

   nghttp2_session* session = nullptr;
   std::map<int32_t, std::shared_ptr<NGHttp2Stream>> streams_;
   int32_t last_id_ = 0;
   size_t request_counter_ = 0;

   /// The largest header section accepted from the peer, see Config::max_header_size.
   size_t max_header_size_ = default_max_header_size;

   //
   // What to advertise as this origin's HTTP/3 endpoint in every response, see
   // server::Config::alt_svc_max_age. Only a server session ever has one.
   //
   std::string alt_svc_;

   Buffer buffer_;
};

// -------------------------------------------------------------------------------------------------

template <typename Stream>
class NGHttp2SessionImpl : public NGHttp2Session
{
protected:
   NGHttp2SessionImpl(std::string_view log_prefix, asio::any_io_executor executor, Stream&& stream)
      : NGHttp2Session(log_prefix, executor), stream_(std::move(stream))
   {
   }

public:
   awaitable<void> send_loop() override;
   awaitable<void> recv_loop() override;
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
   using super::max_header_size_;
   using super::session;
   using super::stream_;

public:
   ServerSession(server::Server::Impl& parent, asio::any_io_executor executor, Stream&& stream);

   awaitable<void> do_session(Buffer&& data) override;

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
   using super::max_header_size_;
   using super::session;
   using super::stream_;

public:
   ClientSession(client::Client::Impl& parent, asio::any_io_executor executor, Stream&& stream);

   awaitable<void> do_session(Buffer&& data) override;

   void on_alt_svc(std::string_view field_value) override { client().on_alt_svc(field_value); }
};

// =================================================================================================

} // namespace anyhttp::nghttp2

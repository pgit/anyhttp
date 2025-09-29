#pragma once

#include "anyhttp/common.hpp"
#include "client_impl.hpp"
#include "nghttp2_stream.hpp"
#include "server_impl.hpp"
#include "session_impl.hpp"

#include <boost/asio.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/stream_traits.hpp>

#include <map>

#include "nghttp2/nghttp2.h"

using namespace std::chrono_literals;

using namespace boost::asio;

namespace anyhttp::nghttp2
{

// =================================================================================================

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
   NGHttp2Session(std::string_view prefix, any_io_executor executor);
   virtual ~NGHttp2Session();

   boost::asio::any_io_executor get_executor() const noexcept override { return m_executor; }
   const std::string& logPrefix() const { return m_logPrefix; }

   std::string logPrefix(int stream_id) const
   {
      if (stream_id)
         return std::format("{}.{}", logPrefix(), stream_id);
      else
         return logPrefix();
   }

   inline std::string logPrefix(const nghttp2_frame* frame) const
   {
      return logPrefix(frame->hd.stream_id);
   }

   // ----------------------------------------------------------------------------------------------

   void async_submit(SubmitHandler&& handler, boost::urls::url url, const Fields& headers) override;

   // ----------------------------------------------------------------------------------------------

   using Resume = void();
   using ResumeHandler = asio::any_completion_handler<Resume>;

   // If set, the send loop has run out of data to send and is waiting for re-activation.
   ResumeHandler m_send_handler;

   // Wait to be resumed via `start_write()`, called from within `send_loop()`.
   template <BOOST_ASIO_COMPLETION_TOKEN_FOR(Resume) CompletionToken = DefaultCompletionToken>
   auto async_wait_send(CompletionToken&& token = CompletionToken())
   {
      return asio::async_initiate<CompletionToken, Resume>(
         [&](ResumeHandler handler)
         {
            assert(!m_send_handler);
            m_send_handler = std::move(handler);
         },
         std::forward<CompletionToken>(token));
   }

   void start_write();

   // ----------------------------------------------------------------------------------------------

   /**
    * Helper function to pass data from #m_buffer to nghttp2, invoked by recv_loop().
    * The buffer will be empty when this function returns. Terminates the session on error.
    */
   void handle_buffer_contents();

   virtual awaitable<void> send_loop() = 0;
   virtual awaitable<void> recv_loop() = 0;

   // ----------------------------------------------------------------------------------------------

   nghttp2_unique_ptr<nghttp2_session_callbacks> setup_callbacks();

   NGHttp2Stream* create_stream(int32_t stream_id);
   NGHttp2Stream* find_stream(int32_t stream_id);
   void close_stream(int32_t stream_id);
   void delete_stream(int32_t stream_id);

public:
   std::string m_logPrefix;
   boost::asio::any_io_executor m_executor;

   nghttp2_session* session = nullptr;
   std::map<int32_t, std::shared_ptr<NGHttp2Stream>> m_streams;
   int32_t m_last_id = 0;
   size_t m_requestCounter = 0;

   Buffer m_buffer;
};

// -------------------------------------------------------------------------------------------------

template <typename Stream>
class NGHttp2SessionImpl : public NGHttp2Session
{
protected:
   NGHttp2SessionImpl(std::string_view logPrefix, any_io_executor executor, Stream&& stream)
      : NGHttp2Session(logPrefix, executor), m_stream(std::move(stream))
   {
   }

public:
   // ~NGHttp2SessionImpl() override;

   // ----------------------------------------------------------------------------------------------

   awaitable<void> send_loop() override;
   awaitable<void> recv_loop() override;
   void destroy(std::shared_ptr<Session::Impl>) noexcept override;

public:
   Stream m_stream;
};

// =================================================================================================

class ServerReference
{
public:
   inline ServerReference(server::Server::Impl& parent) : m_server(&parent) {}
   server::Server::Impl& server()
   {
      assert(m_server);
      return *m_server;
   }

private:
   server::Server::Impl* m_server = nullptr;
};

template <typename Stream>
class ServerSession : public ServerReference, public NGHttp2SessionImpl<Stream>
{
   using super = NGHttp2SessionImpl<Stream>;

   // FIXME: maybe use CRTP or something similar to avoid this?
   using super::handle_buffer_contents;
   using super::logPrefix;
   using super::recv_loop;
   using super::send_loop;

   using super::m_buffer;
   using super::m_stream;
   using super::session;

public:
   ServerSession(server::Server::Impl& parent, any_io_executor executor, Stream&& stream);

   // void async_submit(SubmitHandler&& handler, boost::urls::url url, const Fields& headers)
   // override;
   awaitable<void> do_session(Buffer&& data) override;
};

// -------------------------------------------------------------------------------------------------

class ClientReference
{
public:
   inline ClientReference(client::Client::Impl& parent) : m_client(&parent) {}
   client::Client::Impl& client()
   {
      assert(m_client);
      return *m_client;
   }

private:
   client::Client::Impl* m_client = nullptr;
};

template <typename Stream>
class ClientSession : public ClientReference, public NGHttp2SessionImpl<Stream>
{
   using super = NGHttp2SessionImpl<Stream>;

   // FIXME: maybe use CRTP or something similar to avoid this?
   using super::handle_buffer_contents;
   using super::logPrefix;
   using super::recv_loop;
   using super::send_loop;

   using super::m_buffer;
   using super::m_stream;
   using super::session;

public:
   ClientSession(client::Client::Impl& parent, any_io_executor executor, Stream&& stream);

   // void async_submit(SubmitHandler&& handler, boost::urls::url url, const Fields& headers)
   // override;
   awaitable<void> do_session(Buffer&& data) override;
};

// =================================================================================================

} // namespace anyhttp::nghttp2
